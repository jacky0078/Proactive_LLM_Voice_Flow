/* SPDX-License-Identifier: MIT */
#pragma once
#include "StackFlowUtil.h"
#include "json.hpp"
#include <ctime>
#include <mutex>
#include <unordered_map>
#define LLM_NO_ERROR std::string("")
#define LLM_NONE     std::string("None")
namespace StackFlows {
struct RequestContext {
    std::string request_id;
    std::string reply_url;
};
nlohmann::json structured_error(const nlohmann::json& error);
class llm_channel_obj {
public:
    std::string unit_name_, request_id_, work_id_, inference_url_, publisher_url_, output_url_;
    bool enoutput_ = false, enstream_ = false;
    llm_channel_obj(const std::string&, const std::string&, const std::string&);
    ~llm_channel_obj();
    void set_output(bool enabled)
    {
        enoutput_ = enabled;
    }
    bool get_output() const
    {
        return enoutput_;
    }
    void set_stream(bool enabled)
    {
        enstream_ = enabled;
    }
    bool get_stream() const
    {
        return enstream_;
    }
    using MessageCallback = std::function<void(const nlohmann::json&, const RequestContext&)>;
    int subscribe_message(const std::string& work_id, MessageCallback callback);
    int subscriber_work_id(const std::string&, const std::function<void(const std::string&, const std::string&)>&);
    void stop_subscriber_work_id(const std::string&);
    void subscriber(const std::string&, const pzmq::msg_callback_fun&);
    void stop_subscriber(const std::string&);
    void set_push_url(const std::string& url)
    {
        output_url_ = url;
    }
    void cear_push_url()
    {
        output_url_.clear();
    }
    int send_raw_to_pub(const std::string&);
    int send_raw_to_usr(const std::string&);
    static int send_raw_for_url(const std::string&, const std::string&);
    int send(const std::string&, const nlohmann::json&, const nlohmann::json&, const std::string& = "");
    int send_with_context(const RequestContext&, const std::string&, const nlohmann::json&,
                          const nlohmann::json& error = nullptr);

private:
    std::unique_ptr<pzmq> publisher_;
    std::unordered_map<std::string, std::unique_ptr<pzmq>> subscribers_;
    std::unordered_map<std::string, std::shared_ptr<pzmq>> outputs_;
    std::mutex output_mutex_;
    std::string input_url(const std::string&);
    int push(const std::string&, const std::string&);
};
}  // namespace StackFlows
