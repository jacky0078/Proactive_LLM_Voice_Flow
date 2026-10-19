#pragma once
#include "network/TcpServer.h"
#include "network/EventLoop.h"
#include "network/TcpConnection.h"
#include "zmq_bus.h"
class TcpSession : public zmq_bus_com {
public:
    explicit TcpSession(const network::TcpConnectionPtr& conn) : conn_(conn)
    {
    }
    void send_data(const std::string& data) override
    {
        if (auto conn = conn_.lock()) {
            network::Buffer buffer;
            buffer.append(data.data(), data.size());
            conn->send(&buffer);
        }
    }

private:
    std::weak_ptr<network::TcpConnection> conn_;
};
