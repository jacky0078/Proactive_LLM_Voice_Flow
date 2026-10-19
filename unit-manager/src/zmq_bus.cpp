/* SPDX-License-Identifier: MIT */
#include "zmq_bus.h"
#include "all.h"
#include "json.hpp"
using namespace StackFlows;
zmq_bus_com::zmq_bus_com() : exit_flage(0), err_count(0), _port(0), json_str_flage_(0)
{
}
void zmq_bus_com::work(const std::string& format, int port)
{
    _port = port;
    char address[256];
    snprintf(address, sizeof(address), format.c_str(), port);
    _zmq_url      = address;
    user_chennal_ = std::make_unique<pzmq>(
        _zmq_url, ZMQ_PULL, [this](pzmq*, const std::shared_ptr<pzmq_data>& p) { send_data(p->string()); });
    exit_flage = 1;
}
void zmq_bus_com::stop()
{
    user_chennal_.reset();
    exit_flage = 0;
}
zmq_bus_com::~zmq_bus_com()
{
    stop();
}
void zmq_bus_com::on_data(const std::string& data)
{
    unit_action_match(_port, data);
}
void zmq_bus_com::send_data(const std::string&)
{
}
int zmq_bus_publisher_push(const std::string& work, const std::string& raw)
{
    std::shared_ptr<unit_data> task;
    SAFE_READING(task, std::shared_ptr<unit_data>, work);
    if (!task) return -1;
    return task->send_msg(raw);
}
void zmq_com_send(int com_id, const std::string& raw)
{
    char address[256];
    snprintf(address, sizeof(address), zmq_c_format.c_str(), com_id);
    pzmq sender(address, ZMQ_PUSH);
    sender.send_data(raw + "\n");
}
void zmq_bus_com::select_json_str(const std::string& data, std::function<void(const std::string&)> emit)
{
    // TCP 是字节流；保留半包，并逐条消费以换行分隔的 JSON。
    for (char c : data) {
        if (json_str_flage_) {
            if (c == '\n') json_str_flage_ = 0;
            continue;
        }
        if (c == '\n') {
            if (!json_str_.empty() && json_str_.back() == '\r') json_str_.pop_back();
            if (!json_str_.empty()) emit(json_str_);
            json_str_.clear();
        } else {
            json_str_.push_back(c);
            if (json_str_.size() > 1024 * 1024) {
                json_str_.clear();
                json_str_flage_ = 1;
                send_data(nlohmann::json{{"request_id", "0"},
                                         {"work_id", "sys"},
                                         {"object", "sys.error"},
                                         {"data", nullptr},
                                         {"error", {{"code", -2}, {"message", "TCP frame exceeds 1 MiB"}}}}
                              .dump() +
                          "\n");
            }
        }
    }
}
