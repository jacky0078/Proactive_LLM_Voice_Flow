/* SPDX-License-Identifier: MIT */
#include "channel.h"
using namespace StackFlows;
nlohmann::json StackFlows::structured_error(const nlohmann::json& error)
{
    if (error.is_null() || (error.is_string() && error.get<std::string>().empty()))
        return {{"code", 0}, {"message", ""}};
    if (error.is_object()) return error;
    if (error.is_string()) {
        auto parsed = nlohmann::json::parse(error.get<std::string>(), nullptr, false);
        if (parsed.is_object()) return parsed;
    }
    return {{"code", -1}, {"message", error.is_string() ? error.get<std::string>() : error.dump()}};
}
llm_channel_obj::llm_channel_obj(const std::string& pub, const std::string& input, const std::string& name)
    : unit_name_(name), inference_url_(input), publisher_url_(pub), publisher_(std::make_unique<pzmq>(pub, ZMQ_PUB))
{
}
llm_channel_obj::~llm_channel_obj()
{
    subscribers_.clear();
}
std::string llm_channel_obj::input_url(const std::string& work)
{
    if (work.empty()) return inference_url_;
    auto url = unit_call("sys", "sql_select", work + ".out_port");
    if (url.empty()) throw std::runtime_error("upstream task not found: " + work);
    return url;
}
int llm_channel_obj::subscribe_message(const std::string& work, MessageCallback callback)
{
    auto url = input_url(work);
    subscriber(url, [this, callback](pzmq*, const std::shared_ptr<pzmq_data>& raw) {
        auto j = nlohmann::json::parse(raw->string());
        RequestContext context{j.at("request_id").get<std::string>(), j.value("zmq_com", output_url_)};
        callback(j, context);
    });
    return 0;
}
int llm_channel_obj::subscriber_work_id(const std::string& work,
                                        const std::function<void(const std::string&, const std::string&)>& callback)
{
    return subscribe_message(work, [this, callback](const nlohmann::json& j, const RequestContext& context) {
        request_id_ = context.request_id;  // Legacy synchronous API. Async nodes use explicit contexts.
        output_url_ = context.reply_url;
        auto data   = j.at("data");
        callback(j.at("object"), data.is_string() ? data.get<std::string>() : data.dump());
    });
}
void llm_channel_obj::subscriber(const std::string& url, const pzmq::msg_callback_fun& cb)
{
    if (subscribers_.count(url)) return;
    subscribers_[url] = std::make_unique<pzmq>(url, ZMQ_SUB, cb);
}
void llm_channel_obj::stop_subscriber(const std::string& url)
{
    if (url.empty())
        subscribers_.clear();
    else
        subscribers_.erase(url);
}
void llm_channel_obj::stop_subscriber_work_id(const std::string& work)
{
    stop_subscriber(input_url(work));
}
int llm_channel_obj::send_raw_to_pub(const std::string& s)
{
    return publisher_->send_data(s);
}
int llm_channel_obj::push(const std::string& url, const std::string& s)
{
    if (url.empty()) return -1;
    std::shared_ptr<pzmq> socket;
    {
        std::lock_guard<std::mutex> l(output_mutex_);
        auto& entry = outputs_[url];
        if (!entry) entry = std::make_shared<pzmq>(url, ZMQ_PUSH);
        socket = entry;
    }
    return socket->send_data(s);
}
int llm_channel_obj::send_raw_to_usr(const std::string& s)
{
    return push(output_url_, s);
}
int llm_channel_obj::send_raw_for_url(const std::string& url, const std::string& s)
{
    pzmq p(url, ZMQ_PUSH);
    return p.send_data(s);
}
int llm_channel_obj::send_with_context(const RequestContext& c, const std::string& object, const nlohmann::json& data,
                                       const nlohmann::json& error)
{
    nlohmann::json j{{"request_id", c.request_id}, {"work_id", work_id_}, {"created", time(nullptr)},
                     {"object", object},           {"data", data},        {"error", structured_error(error)},
                     {"zmq_com", c.reply_url}};
    auto wire = j.dump() + "\n";
    int rc    = send_raw_to_pub(wire);
    if (enoutput_ && push(c.reply_url, wire)) rc = -1;
    return rc;
}
int llm_channel_obj::send(const std::string& object, const nlohmann::json& data, const nlohmann::json& error,
                          const std::string& work)
{
    return send_with_context({request_id_, output_url_}, object, data, error);
}
