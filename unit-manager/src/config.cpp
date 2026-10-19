/* SPDX-License-Identifier: MIT */
#include "all.h"
#include "json.hpp"
#include <fstream>
#include <regex>
void load_default_config(const std::string& path)
{
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open manager config: " + path);
    nlohmann::json config;
    file >> config;
    int port  = config.at("config_tcp_server").get<int>();
    int first = config.at("config_zmq_min_port").get<int>();
    int last  = config.at("config_zmq_max_port").get<int>();
    if (port < 1 || port > 65535 || first < 0 || last <= first + 1 || last > 65536)
        throw std::runtime_error("invalid port/address pool configuration");
    for (auto key : {"config_zmq_s_format", "config_zmq_c_format"}) {
        auto format = config.at(key).get<std::string>();
        // 该单机原型只接受一个整数占位符，避免将配置交给不受约束的 printf。
        if (!std::regex_match(format, std::regex("ipc://[^%]+%[id][^%]*")))
            throw std::runtime_error("expected local IPC format with exactly one %i or %d");
        if (format.size() > 160) throw std::runtime_error("IPC format too long");
    }
    for (auto it = config.begin(); it != config.end(); ++it) {
        if (it.value().is_number_integer())
            key_sql[it.key()] = it.value().get<int>();
        else if (it.value().is_string())
            key_sql[it.key()] = it.value().get<std::string>();
    }
}
