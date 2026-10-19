
#include <unordered_map>
#include <unistd.h>
#include <chrono>
#include <any>
#include <cstring>
#include <ctime>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "all.h"
#include "session.h"
#include "zmq_bus.h"
#include <boost/any.hpp>
#include "json.hpp"
#include <variant>
#include <sys/signalfd.h>
#include <signal.h>
#include "network/Channel.h"

std::atomic<int> counter_port(8000);
network::EventLoop loop;
std::unique_ptr<network::TcpServer> server;

void onConnection(const network::TcpConnectionPtr &conn)
{
    if (conn->connected()) {
        std::shared_ptr<TcpSession> session = std::make_shared<TcpSession>(conn);
        conn->setContext(session);
        session->work(zmq_s_format, counter_port.fetch_add(1));

        if (counter_port > 65535) {
            counter_port.store(8000);
        }
    } else {
        try {
            auto session = boost::any_cast<std::shared_ptr<TcpSession>>(conn->getContext());
            session->stop();
            conn->setContext(boost::any());
        } catch (const boost::bad_any_cast &e) {
            std::cerr << "Bad any_cast: " << e.what() << std::endl;
        }
    }
}

void onMessage(const network::TcpConnectionPtr &conn, network::Buffer *buf)
{
    std::string msg(buf->retrieveAllAsString());

    try {
        auto session = boost::any_cast<std::shared_ptr<TcpSession>>(conn->getContext());

        session->select_json_str(msg, std::bind(&TcpSession::on_data, session, std::placeholders::_1));
    } catch (const boost::bad_any_cast &e) {
        std::cerr << "Type cast error: " << e.what() << std::endl;
    }
}

void tcp_work()
{
    int listenport = 0;
    SAFE_READING(listenport, int, "config_tcp_server");
    network::InetAddress listenAddr(listenport);
    server = std::make_unique<network::TcpServer>(&loop, listenAddr, "ZMQBridge");

    server->setConnectionCallback(onConnection);
    server->setMessageCallback(onMessage);
    server->setThreadNum(2);

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) throw std::runtime_error("signalfd failed");
    network::Channel signal_channel(&loop, signal_fd);
    signal_channel.setReadCallback([signal_fd] {
        signalfd_siginfo signal_info;
        if (read(signal_fd, &signal_info, sizeof(signal_info)) > 0) loop.quit();
    });
    signal_channel.enableReading();
    server->start();
    loop.loop();
    signal_channel.disableAll();
    signal_channel.remove();
    close(signal_fd);
    server.reset();
}
