/* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT */
#include "StackFlow.h"
#include <iostream>
using namespace StackFlows;
StackFlow::StackFlow(const std::string& name, bool defer_start) : unit_name_(name), rpc_(std::make_unique<pzmq>(name))
{
    if (!defer_start) start_events();
}

void StackFlow::start_events()
{
    if (events_.joinable()) return;
    for (const std::string action : {"setup", "exit", "pause", "taskinfo"})
        rpc_->register_rpc_action(action, [this, action](pzmq*, const std::shared_ptr<pzmq_data>& data) {
            Event event{action, data->get_param(0), data->get_param(1)};
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (stopped_) throw std::runtime_error("node stopping");
                if (queue_.size() >= 32) throw std::runtime_error("lifecycle queue full");
                queue_.push_back(std::move(event));
            }
            cv_.notify_one();
            return "None";
        });
    events_ = std::thread([this] { event_loop(); });
}
void StackFlow::stop_events()
{
    rpc_.reset();
    {
        std::lock_guard<std::mutex> l(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
    if (events_.joinable()) events_.join();
}
StackFlow::~StackFlow()
{
    stop_events();
    while (!llm_task_channel_.empty()) {
        auto n = llm_task_channel_.begin()->first;
        try {
            sys_release_unit(n, "");
        } catch (...) {
            llm_task_channel_.erase(n);
        }
    }
}
void StackFlow::event_loop()
{
    for (;;) {
        Event e;
        {
            std::unique_lock<std::mutex> l(mutex_);
            cv_.wait(l, [&] { return stopped_ || !queue_.empty(); });
            if (stopped_ && queue_.empty()) return;
            e = std::move(queue_.front());
            queue_.pop_front();
        }
        request_id_  = sample_json_str_get(e.raw, "request_id");
        out_zmq_url_ = e.url;
        try {
            if (e.action == "setup")
                setup(e.url, e.raw);
            else if (e.action == "exit")
                exit(e.url, e.raw);
            else if (e.action == "pause")
                pause(e.url, e.raw);
            else
                taskinfo(e.url, e.raw);
        } catch (const std::exception& error) {
            send("sys.error", nullptr, {{"code", -10}, {"message", error.what()}},
                 sample_json_str_get(e.raw, "work_id"));
        }
    }
}
int StackFlow::setup(const std::string& url, const std::string& raw)
{
    int n     = sys_register_unit(unit_name_);
    auto work = sample_get_work_id(n, unit_name_);
    try {
        auto channel = get_channel(n);
        channel->set_push_url(url);
        channel->request_id_ = request_id_;
        channel->work_id_    = work;
        if (setup(work, sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data")))
            sys_release_unit(n, work);
    } catch (...) {
        sys_release_unit(n, work);
        throw;
    }
    return 0;
}
int StackFlow::setup(const std::string&, const std::string&, const std::string&)
{
    throw std::runtime_error("setup not implemented");
}
int StackFlow::exit(const std::string&, const std::string& raw)
{
    auto work = sample_json_str_get(raw, "work_id");
    get_channel(work);  // Fail before touching business state for an unknown task.
    if (exit(work, sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data")) == 0) {
        if (!sys_release_unit(-1, work)) throw std::runtime_error("task release failed");
        send("sys.exit", nullptr, nullptr, work);
    }
    return 0;
}
int StackFlow::exit(const std::string&, const std::string&, const std::string&)
{
    return 0;
}
void StackFlow::pause(const std::string&, const std::string& raw)
{
    pause(sample_json_str_get(raw, "work_id"), sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data"));
}
void StackFlow::pause(const std::string&, const std::string&, const std::string&)
{
    throw std::runtime_error("pause not implemented");
}
void StackFlow::taskinfo(const std::string&, const std::string& raw)
{
    taskinfo(sample_json_str_get(raw, "work_id"), sample_json_str_get(raw, "object"), sample_json_str_get(raw, "data"));
}
void StackFlow::taskinfo(const std::string& work, const std::string&, const std::string&)
{
    get_channel(work);
    send("sys.taskinfo", {{"work_id", work}}, nullptr, work);
}
int StackFlow::send(const std::string& object, const nlohmann::json& data, const nlohmann::json& error,
                    const std::string& work, const std::string& url)
{
    nlohmann::json j{{"request_id", request_id_}, {"work_id", work}, {"created", time(nullptr)},
                     {"object", object},          {"data", data},    {"error", structured_error(error)}};
    return llm_channel_obj::send_raw_for_url(url.empty() ? out_zmq_url_ : url, j.dump() + "\n");
}
int StackFlow::sys_register_unit(const std::string& name)
{
    int n = -1;
    std::string pub, input;
    unit_call("sys", "register_unit", name, [&](const std::shared_ptr<pzmq_data>& reply) {
        n              = std::stoi(reply->get_param(0));
        auto addresses = reply->get_param(1);
        pub            = reply->get_param(0, addresses);
        input          = reply->get_param(1, addresses);
    });
    try {
        llm_task_channel_[n] = std::make_shared<llm_channel_obj>(pub, input, name);
    } catch (...) {
        unit_call("sys", "release_unit", sample_get_work_id(n, name));
        throw;
    }
    return n;
}
bool StackFlow::sys_release_unit(int n, const std::string& work)
{
    auto id = work.empty() ? sample_get_work_id(n, unit_name_) : work;
    auto rc = unit_call("sys", "release_unit", id);
    llm_task_channel_.erase(sample_get_work_id_num(id));
    return rc == "Success";
}
std::string StackFlow::sys_sql_select(const std::string& key)
{
    return unit_call("sys", "sql_select", key);
}
void StackFlow::sys_sql_set(const std::string& key, const std::string& val)
{
    unit_call("sys", "sql_set", nlohmann::json{{"key", key}, {"val", val}}.dump());
}
void StackFlow::sys_sql_unset(const std::string& key)
{
    unit_call("sys", "sql_unset", key);
}
