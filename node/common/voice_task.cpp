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
    if (supports_cancel()) canceller_ = std::thread([this] { cancel_loop(); });
}
void VoiceTask::stop()
{
    if (stopping_.exchange(true)) return;
    input_cv_.notify_all();
    output_cv_.notify_all();
    abort();
    if (worker_.joinable()) worker_.join();
    input_cv_.notify_all();
    if (canceller_.joinable()) canceller_.join();
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
    return input_cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                              [this] { return stopping() || cancelled(); });
}
void VoiceTask::submit(const Json& body, const RequestContext& context)
{
    if (stopping()) return;
    if (body.value("object", "") == "llm.cancel") {
        cancel_request(body, context);
        return;
    }
    if (body.value("object", "") == "sys.probe") {
        auto data = body.at("data");
        if (!data.is_object()) data = Json::object();
        data["protocol_version"]     = 2;
        data["request_cancellation"] = supports_cancel();
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            data["idle"] = active_request_.empty() && !cancelling_;
        }
        emit(context, "sys.probe", data);
        return;
    }
    std::string error;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (cancelling_)
            error = "task cancellation in progress";
        else if (paused_)
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
        queue_output({context, "sys.error", nullptr, {{"code", -20}, {"message", error}}});
    } else {
        if (body.value("ack_requested", false)) emit(context, "sys.accepted", {{"accepted", true}});
        input_cv_.notify_all();
    }
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
void VoiceTask::queue_output(Output output)
{
    std::unique_lock<std::mutex> lock(output_mutex_);
    output_cv_.wait(lock, [&] { return stopping() || output_.size() < 128; });
    if (stopping()) return;
    output_.push_back(std::move(output));
    output_cv_.notify_all();
}
void VoiceTask::emit(const RequestContext& context, const std::string& object, const Json& data, bool terminal)
{
    queue_output({context, object, data, nullptr});
    if (terminal) finish_request(context.request_id);
    input_cv_.notify_all();
}
void VoiceTask::fail(const RequestContext& context, const std::string& message)
{
    queue_output({context, "sys.error", nullptr, {{"code", -21}, {"message", message}}});
    finish_request(context.request_id);
    input_cv_.notify_all();
}
void VoiceTask::cancel_request(const Json& body, const RequestContext& context)
{
    const auto& data = body.at("data");
    auto target      = data.value("request_id", "");
    std::string error;
    bool idle = false;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (!supports_cancel() || target.empty())
            error = "request cancellation unsupported or target missing";
        else if (cancelling_)
            error = "cancellation already in progress";
        else if (active_request_.empty())
            idle = true;
        else if (active_request_ != target)
            error = "cancellation target does not match active request";
        else {
            cancellation_     = Cancellation{context, target, data};
            cancelling_       = true;
            cancel_requested_ = true;
        }
    }
    if (!error.empty())
        queue_output({context, "sys.error", nullptr, {{"code", -20}, {"message", error}}});
    else if (idle)
        emit(context, "llm.cancelled", {{"request_id", target}, {"state", "not_active"}});
    input_cv_.notify_all();
}
void VoiceTask::cancel_loop()
{
    for (;;) {
        Cancellation command;
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [&] { return stopping() || cancellation_.has_value(); });
            if (stopping()) return;
            command = std::move(*cancellation_);
            cancellation_.reset();
        }
        // 控制线程不排在推理后面；重复取消覆盖 SDK 尚未进入 run 的竞态。
        for (;;) {
            cancel_active();
            std::unique_lock<std::mutex> lock(input_mutex_);
            if (input_cv_.wait_for(lock, std::chrono::milliseconds(10),
                                   [&] { return stopping() || active_request_ != command.target; }))
                break;
        }
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            cancelling_       = false;
            cancel_requested_ = false;
        }
        emit(command.context, "llm.cancelled", {{"request_id", command.target}, {"state", "cancelled"}});
    }
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
            if (cancelled()) throw std::runtime_error("request cancelled");
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
            output_cv_.notify_all();
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
