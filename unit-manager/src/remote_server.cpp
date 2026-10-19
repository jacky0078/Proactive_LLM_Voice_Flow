/* SPDX-License-Identifier: MIT */
#include "remote_server.h"
#include "remote_action.h"
#include "zmq_bus.h"
#include "StackFlowUtil.h"
#include "json.hpp"
#include <mutex>
#include <regex>
using namespace StackFlows;
namespace {
std::unique_ptr<pzmq> sys_rpc_server;
std::vector<bool> allocated;
int port_start   = 5010;
int next_work_id = 0;
std::mutex registry_mutex;
std::string address(int number)
{
    char buffer[256];
    if (snprintf(buffer, sizeof(buffer), zmq_s_format.c_str(), number) >= static_cast<int>(sizeof(buffer)))
        throw std::runtime_error("IPC address too long");
    return buffer;
}
std::string allocate_unit(const std::string& name)
{
    if (!std::regex_match(name, std::regex("[a-z][a-z0-9_]*"))) throw std::runtime_error("invalid unit name");
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::vector<int> slots;
    for (size_t i = 0; i < allocated.size() && slots.size() < 2; ++i)
        if (!allocated[i]) slots.push_back(i);
    if (slots.size() != 2) throw std::runtime_error("task address pool exhausted");
    auto task         = std::make_shared<unit_data>();
    task->port_       = next_work_id++;
    task->work_id     = name + "." + std::to_string(task->port_);
    task->output_slot = slots[0];
    task->input_slot  = slots[1];
    task->output_url  = address(port_start + slots[0]) + "." + name + ".output_url";
    task->init_zmq(address(port_start + slots[1]) + "." + name + ".inference_url");
    allocated[slots[0]] = allocated[slots[1]] = true;
    SAFE_SETTING(task->work_id, task);
    SAFE_SETTING(task->work_id + ".out_port", task->output_url);
    return pzmq_data::set_param(std::to_string(task->port_),
                                pzmq_data::set_param(task->output_url, task->inference_url));
}
std::string release_unit(const std::string& work)
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::shared_ptr<unit_data> task;
    SAFE_READING(task, std::shared_ptr<unit_data>, work);
    if (!task) throw std::runtime_error("task not registered: " + work);
    SAFE_ERASE(work);
    SAFE_ERASE(work + ".out_port");
    allocated.at(task->output_slot) = allocated.at(task->input_slot) = false;
    return "Success";
}
}  // namespace
void remote_server_work()
{
    int port_end = 5555;
    SAFE_READING(port_start, int, "config_zmq_min_port");
    SAFE_READING(port_end, int, "config_zmq_max_port");
    allocated.assign(port_end - port_start, false);
    sys_rpc_server = std::make_unique<pzmq>("sys");
    sys_rpc_server->register_rpc_action(
        "register_unit", [](pzmq*, const std::shared_ptr<pzmq_data>& p) { return allocate_unit(p->string()); });
    sys_rpc_server->register_rpc_action(
        "release_unit", [](pzmq*, const std::shared_ptr<pzmq_data>& p) { return release_unit(p->string()); });
    sys_rpc_server->register_rpc_action("sql_select", [](pzmq*, const std::shared_ptr<pzmq_data>& p) {
        std::string value;
        SAFE_READING(value, std::string, p->string());
        return value;
    });
    sys_rpc_server->register_rpc_action("sql_set", [](pzmq*, const std::shared_ptr<pzmq_data>& p) {
        auto j     = nlohmann::json::parse(p->string());
        auto key   = j.at("key").get<std::string>();
        auto value = j.at("val").get<std::string>();
        SAFE_SETTING(key, value);
        return "Success";
    });
    sys_rpc_server->register_rpc_action("sql_unset", [](pzmq*, const std::shared_ptr<pzmq_data>& p) {
        SAFE_ERASE(p->string());
        return "Success";
    });
}
void remote_server_stop_work()
{
    sys_rpc_server.reset();
    key_sql.clear();
}
void unit_action_match(int com_id, const std::string& raw)
{
    std::string request = "0", work = "sys";
    try {
        auto j      = nlohmann::json::parse(raw);
        request     = j.at("request_id").get<std::string>();
        work        = j.at("work_id").get<std::string>();
        auto action = j.at("action").get<std::string>();
        j.at("object").get<std::string>();
        j.at("data");
        if (request.empty() || work.empty() || action.empty()) throw std::runtime_error("empty request fields");
        if (action == "inference") {
            char reply[256];
            snprintf(reply, sizeof(reply), zmq_c_format.c_str(), com_id);
            j["zmq_com"] = reply;
            if (zmq_bus_publisher_push(work, j.dump()))
                throw std::runtime_error("task not found or input publish failed");
        } else if (remote_call(com_id, raw))
            throw std::runtime_error("node RPC failed or timed out");
    } catch (const std::exception& error) {
        nlohmann::json j{{"request_id", request},    {"work_id", work},
                         {"created", time(nullptr)}, {"object", "sys.error"},
                         {"data", nullptr},          {"error", {{"code", -2}, {"message", error.what()}}}};
        zmq_com_send(com_id, j.dump());
    }
}
