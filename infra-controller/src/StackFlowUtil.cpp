/* SPDX-License-Identifier: MIT */
#include "StackFlowUtil.h"
#include "json.hpp"
#include <filesystem>
#include <stdexcept>
using namespace StackFlows;
std::string StackFlows::sample_json_str_get(const std::string& raw, const std::string& key)
{
    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (!j.is_object() || !j.contains(key)) return {};
    return j[key].is_string() ? j[key].get<std::string>() : j[key].dump();
}
int StackFlows::sample_get_work_id_num(const std::string& work)
{
    auto pos = work.find('.');
    if (pos == std::string::npos) return WORK_ID_NONE;
    size_t used = 0;
    int number  = std::stoi(work.substr(pos + 1), &used);
    if (used != work.size() - pos - 1 || number < 0) throw std::invalid_argument("invalid work_id");
    return number;
}
std::string StackFlows::sample_get_work_id_name(const std::string& work)
{
    return work.substr(0, work.find('.'));
}
std::string StackFlows::sample_get_work_id(int n, const std::string& name)
{
    return name + "." + std::to_string(n);
}
bool StackFlows::decode_stream(const std::string& in, std::string& out, std::unordered_map<int, std::string>& buffer)
{
    auto j    = nlohmann::json::parse(in);
    int index = j.at("index").get<int>();
    if (index < 0 || buffer.count(index)) {
        buffer.clear();
        throw std::runtime_error("invalid/duplicate stream index");
    }
    buffer[index] = j.at("delta").get<std::string>();
    if (!j.at("finish").get<bool>()) return false;
    out.clear();
    if (buffer.size() != static_cast<size_t>(index + 1)) {
        buffer.clear();
        throw std::runtime_error("missing stream fragment");
    }
    for (int i = 0; i <= index; ++i) out += buffer.at(i);
    buffer.clear();
    return true;
}
void StackFlows::unit_call(const std::string& unit, const std::string& action, const std::string& data,
                           std::function<void(const std::shared_ptr<pzmq_data>&)> callback)
{
    pzmq rpc(unit);
    int rc = rpc.call_rpc_action(action, data, [&](pzmq*, const std::shared_ptr<pzmq_data>& reply) {
        if (reply->string().rfind("RPC_ERROR:", 0) == 0) throw std::runtime_error(reply->string());
        if (callback) callback(reply);
    });
    if (rc) throw std::runtime_error("RPC failed or timed out: " + unit + "." + action);
}
std::string StackFlows::unit_call(const std::string& unit, const std::string& action, const std::string& data)
{
    std::string result;
    unit_call(unit, action, data, [&](const std::shared_ptr<pzmq_data>& p) { result = p->string(); });
    return result;
}
bool StackFlows::file_exists(const std::string& path)
{
    return std::filesystem::is_regular_file(path);
}
void StackFlows::unicode_to_utf8(unsigned int cp, char* out, int* size)
{
    if (cp < 0x80) {
        out[0] = cp;
        *size  = 1;
    } else if (cp < 0x800) {
        out[0] = 0xc0 | (cp >> 6);
        out[1] = 0x80 | (cp & 63);
        *size  = 2;
    } else if (cp < 0x10000) {
        out[0] = 0xe0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 63);
        out[2] = 0x80 | (cp & 63);
        *size  = 3;
    } else {
        out[0] = 0xf0 | (cp >> 18);
        out[1] = 0x80 | ((cp >> 12) & 63);
        out[2] = 0x80 | ((cp >> 6) & 63);
        out[3] = 0x80 | (cp & 63);
        *size  = 4;
    }
}
