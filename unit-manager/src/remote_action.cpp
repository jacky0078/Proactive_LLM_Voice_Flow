/* SPDX-License-Identifier: MIT */
#include "all.h"
#include "remote_action.h"
#include "StackFlowUtil.h"
#include "json.hpp"
int remote_call(int com_id, const std::string& raw)
{
    auto j    = nlohmann::json::parse(raw);
    auto work = j.at("work_id").get<std::string>();
    char address[256];
    snprintf(address, sizeof(address), zmq_c_format.c_str(), com_id);
    auto result = StackFlows::unit_call(work.substr(0, work.find('.')), j.at("action"),
                                        StackFlows::pzmq_data::set_param(address, raw));
    if (result != "None") throw std::runtime_error("unexpected node RPC acknowledgement: " + result);
    return 0;
}
