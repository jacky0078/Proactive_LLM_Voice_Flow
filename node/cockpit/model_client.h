#pragma once
#include "mailbox.h"
#include "json.hpp"
#include <functional>
#include <string>
#include <thread>

namespace cockpit {
// Socket 的创建、收发和关闭均在本线程；核心只提交有界命令。
class ModelClient {
public:
    using Json = nlohmann::json;
    ModelClient(std::string host, uint16_t port, std::function<bool(Json)> receive);
    ~ModelClient();
    void start();
    void stop();
    bool send(Json request);
    bool connected() const
    {
        return connected_;
    }

private:
    std::string host_;
    uint16_t port_;
    std::function<bool(Json)> receive_;
    Mailbox<Json> outgoing_;
    WakeFd wake_;
    std::thread thread_;
    std::atomic<bool> stopping_{false}, connected_{false};
    void run();
};
}  // namespace cockpit
