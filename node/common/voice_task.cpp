#include "voice_task.h"
#include <algorithm>
#include <iostream>
using namespace StackFlows;
VoiceTask::VoiceTask(std::shared_ptr<llm_channel_obj> channel, const Json& config)
    : config_(config), channel_(std::move(channel))
{
}
VoiceTask::~VoiceTask()
{
    stop();
}
void VoiceTask::start()
{
    publisher_ = std::thread([this] { publish(); });
    worker_    = std::thread([this] { run(); });
}
void VoiceTask::stop()
{
    if (stopping_.exchange(true)) return;
    abort();
    input_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_stopped_ = true;
    }
    output_cv_.notify_all();
    if (publisher_.joinable()) publisher_.join();
}
bool VoiceTask::wait_or_stop(int milliseconds)
{
    std::unique_lock<std::mutex> lock(input_mutex_);
    return input_cv_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stopping(); });
}
void VoiceTask::submit(const Json& body, const RequestContext& context)
{
    if (stopping()) return;
    if (body.value("object", "") == "sys.probe") {
        emit(context, "sys.probe", body.at("data"));
        return;
    }
    std::string error;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (paused_)
            error = "task paused";
        else if (std::find(completed_.begin(), completed_.end(), context.request_id) != completed_.end())
            return;
        else if (!active_request_.empty() && (active_request_ != context.request_id || !accepts_fragments()))
            error = "task busy: only one request may be active";
        else if (input_.size() >= 128)
            error = "task input queue full";
        else {
            active_request_ = context.request_id;
            input_.push_back({body, context});
        }
    }
    if (!error.empty()) {
        // 拒绝新请求不能清除正在执行的请求。
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_.push_back({context, "sys.error", nullptr, {{"code", -20}, {"message", error}}});
        output_cv_.notify_one();
    } else
        input_cv_.notify_one();
}
void VoiceTask::finish_request(const std::string& request)
{
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (active_request_ == request) active_request_.clear();
    completed_.push_back(request);
    if (completed_.size() > 64) completed_.pop_front();
    input_.erase(std::remove_if(input_.begin(), input_.end(),
                                [&](const VoiceMessage& message) { return message.context.request_id == request; }),
                 input_.end());
}
void VoiceTask::emit(const RequestContext& context, const std::string& object, const Json& data, bool terminal)
{
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_.push_back({context, object, data, nullptr});
    }
    if (terminal) finish_request(context.request_id);
    output_cv_.notify_one();
}
void VoiceTask::fail(const RequestContext& context, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_.push_back({context, "sys.error", nullptr, {{"code", -21}, {"message", message}}});
    }
    finish_request(context.request_id);
    output_cv_.notify_one();
}
void VoiceTask::run()
{
    while (!stopping()) {
        VoiceMessage message;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping() || !input_.empty(); });
            if (stopping()) break;
            if (input_.empty()) {
                lock.unlock();
                on_idle();
                continue;
            }
            message = std::move(input_.front());
            input_.pop_front();
        }
        try {
            if (message.body.contains("error") && message.body["error"].value("code", 0) != 0)
                throw std::runtime_error("upstream: " + message.body["error"].value("message", "failed"));
            process(message);
        } catch (const std::exception& error) {
            reset_request();
            fail(message.context, error.what());
        }
    }
}
void VoiceTask::publish()
{
    for (;;) {
        Output output;
        {
            std::unique_lock<std::mutex> lock(output_mutex_);
            output_cv_.wait(lock, [this] { return output_stopped_ || !output_.empty(); });
            if (output_.empty()) return;
            output = std::move(output_.front());
            output_.pop_front();
        }
        try {
            if (channel_->send_with_context(output.context, output.object, output.data, output.error))
                std::cerr << "result delivery failed: " << output.context.request_id << '\n';
        } catch (const std::exception& error) {
            std::cerr << "result delivery: " << error.what() << '\n';
        }
    }
}
VoiceNode::VoiceNode(const std::string& name, VoiceTaskFactory factory)
    : StackFlow(name, true), factory_(std::move(factory))
{
    // 派生成员就绪后再开放 RPC，避免启动请求访问尚未构造的任务工厂。
    start_events();
}
VoiceNode::~VoiceNode()
{
    stop_events();
    if (channel_) channel_->stop_subscriber("");
    if (task_) task_->stop();
    task_.reset();
    channel_.reset();
}
int VoiceNode::setup(const std::string& work, const std::string&, const std::string& raw)
{
    if (task_) throw std::runtime_error("node already has an active task");
    auto config  = Json::parse(raw);
    auto channel = get_channel(work);
    channel->set_output(config.value("enoutput", true));
    channel->set_stream(true);
    auto task = factory_(channel, config);
    task->start();
    std::weak_ptr<VoiceTask> weak = task;
    auto callback                 = [weak](const Json& body, const RequestContext& context) {
        if (auto task = weak.lock()) task->submit(body, context);
    };
    try {
        channel->subscribe_message("", callback);
        auto upstream = config.value("input", "");
        if (!upstream.empty()) channel->subscribe_message(upstream, callback);
    } catch (...) {
        channel->stop_subscriber("");
        task->stop();
        throw;
    }
    task_    = std::move(task);
    channel_ = channel;
    send("sys.setup", {{"backend", config.value("backend", unit_name_)}}, nullptr, work);
    return 0;
}
int VoiceNode::exit(const std::string& work, const std::string&, const std::string&)
{
    if (!channel_ || channel_->work_id_ != work) throw std::runtime_error("unknown active task");
    channel_->stop_subscriber("");
    task_->stop();
    task_.reset();
    channel_.reset();
    return 0;
}
void VoiceNode::pause(const std::string& work, const std::string&, const std::string& raw)
{
    if (!channel_ || channel_->work_id_ != work) throw std::runtime_error("unknown active task");
    auto config = Json::parse(raw);
    bool paused = config.value("paused", true);
    task_->set_paused(paused);
    send("sys.pause", {{"paused", paused}}, nullptr, work);
}
