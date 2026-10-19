/* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT */
#pragma once
#include "channel.h"
#include <condition_variable>
#include <deque>
#include <thread>
#include <type_traits>
namespace StackFlows {
class StackFlow {
public:
    explicit StackFlow(const std::string& unit_name, bool defer_start = false);
    void start_events();
    virtual ~StackFlow();
    // Derived destructors must call this before destroying task state.
    void stop_events();
    std::string unit_name_, request_id_, out_zmq_url_;
    std::unordered_map<int, std::shared_ptr<llm_channel_obj>> llm_task_channel_;
    template <typename T>
    std::shared_ptr<llm_channel_obj> get_channel(T work)
    {
        int n;
        if constexpr (std::is_same<T, int>::value)
            n = work;
        else
            n = sample_get_work_id_num(work);
        return llm_task_channel_.at(n);
    }
    virtual int setup(const std::string& url, const std::string& raw);
    virtual int setup(const std::string& work, const std::string& object, const std::string& data);
    virtual int exit(const std::string& url, const std::string& raw);
    virtual int exit(const std::string& work, const std::string& object, const std::string& data);
    virtual void pause(const std::string& url, const std::string& raw);
    virtual void pause(const std::string& work, const std::string& object, const std::string& data);
    virtual void taskinfo(const std::string& url, const std::string& raw);
    virtual void taskinfo(const std::string& work, const std::string& object, const std::string& data);
    int send(const std::string&, const nlohmann::json&, const nlohmann::json&, const std::string&,
             const std::string& = "");
    std::string sys_sql_select(const std::string&);
    void sys_sql_set(const std::string&, const std::string&);
    void sys_sql_unset(const std::string&);
    int sys_register_unit(const std::string&);
    bool sys_release_unit(int, const std::string&);
    template <typename T>
    bool sys_release_unit(T work)
    {
        if constexpr (std::is_same<T, int>::value)
            return sys_release_unit(work, "");
        else
            return sys_release_unit(-1, work);
    }

private:
    struct Event {
        std::string action, url, raw;
    };
    std::unique_ptr<pzmq> rpc_;
    std::thread events_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
    bool stopped_ = false;
    void event_loop();
};
}  // namespace StackFlows
