/* SPDX-License-Identifier: MIT
 * One owner thread per persistent socket. RPC client sockets are call-local.
 */
#pragma once
#include "pzmq_data.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#define ZMQ_RPC_FUN  (ZMQ_REP | 0x80)
#define ZMQ_RPC_CALL (ZMQ_REQ | 0x80)
namespace StackFlows {
class pzmq {
public:
    using msg_callback_fun = std::function<void(pzmq*, const std::shared_ptr<pzmq_data>&)>;
    using rpc_callback_fun = std::function<std::string(pzmq*, const std::shared_ptr<pzmq_data>&)>;
    explicit pzmq(const std::string& name) : url_(rpc_url(name))
    {
    }
    pzmq(const std::string& url, int mode, msg_callback_fun callback = {})
        : url_(url), mode_(mode), callback_(std::move(callback))
    {
        start();
    }
    pzmq(const pzmq&) = delete;
    pzmq& operator=(const pzmq&) = delete;
    ~pzmq()
    {
        close_zmq();
    }
    void set_timeout(int ms)
    {
        timeout_ = ms;
    }
    int get_timeout() const
    {
        return timeout_;
    }
    static std::string rpc_url(const std::string& name)
    {
        if (name.find("://") != std::string::npos) return name;
        const char* dir = std::getenv("EDGE_RPC_DIR");
        return "ipc://" + std::string(dir ? dir : "/tmp") + "/rpc." + name;
    }
    void register_rpc_action(const std::string& name, rpc_callback_fun callback)
    {
        {
            std::lock_guard<std::mutex> l(mutex_);
            actions_[name] = std::move(callback);
        }
        if (!thread_.joinable()) {
            mode_ = ZMQ_RPC_FUN;
            start();
        }
    }
    int call_rpc_action(const std::string& action, const std::string& data, msg_callback_fun callback)
    {
        void* ctx    = zmq_ctx_new();
        void* socket = zmq_socket(ctx, ZMQ_REQ);
        int linger = 0, timeout = timeout_;
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(socket, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
        zmq_setsockopt(socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
        int rc      = -1;
        auto result = std::make_shared<pzmq_data>();
        if (zmq_connect(socket, url_.c_str()) == 0 &&
            zmq_send(socket, action.data(), action.size(), ZMQ_SNDMORE) >= 0 &&
            zmq_send(socket, data.data(), data.size(), 0) >= 0 && zmq_msg_recv(result->get(), socket, 0) >= 0)
            rc = 0;
        zmq_close(socket);
        zmq_ctx_term(ctx);
        if (!rc && callback) callback(this, result);
        return rc;
    }
    int send_data(const std::string& data)
    {
        auto job    = std::make_shared<Send>();
        job->data   = data;
        auto future = job->done.get_future();
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (stopping_ || (mode_ != ZMQ_PUB && mode_ != ZMQ_PUSH)) return -1;
            sends_.push_back(job);
        }
        cv_.notify_one();
        return future.get();
    }
    int send_data(const void* data, size_t size)
    {
        return send_data(std::string(static_cast<const char*>(data), size));
    }
    void close_zmq()
    {
        {
            std::lock_guard<std::mutex> l(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    struct Send {
        std::string data;
        std::promise<int> done;
    };
    std::string url_;
    int mode_ = 0;
    std::atomic<int> timeout_{3000};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Send>> sends_;
    std::unordered_map<std::string, rpc_callback_fun> actions_;
    msg_callback_fun callback_;
    void start()
    {
        std::promise<std::string> ready;
        auto result = ready.get_future();
        thread_     = std::thread([this, ready = std::move(ready)]() mutable { run(std::move(ready)); });
        auto error  = result.get();
        if (!error.empty()) {
            close_zmq();
            throw std::runtime_error(error);
        }
    }
    void run(std::promise<std::string> ready)
    {
        void* ctx        = zmq_ctx_new();
        void* socket     = zmq_socket(ctx, mode_ & ~0x80);
        const bool binds = mode_ == ZMQ_PUB || mode_ == ZMQ_PULL || mode_ == ZMQ_RPC_FUN;
        int linger = 1000, timeout = timeout_, receive_timeout = 50;
        zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(socket, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
        zmq_setsockopt(socket, ZMQ_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        if (mode_ == ZMQ_SUB) zmq_setsockopt(socket, ZMQ_SUBSCRIBE, "", 0);
        std::string error;
        try {
            if (binds && url_.rfind("ipc://", 0) == 0)
                std::filesystem::create_directories(std::filesystem::path(url_.substr(6)).parent_path());
            if ((binds ? zmq_bind(socket, url_.c_str()) : zmq_connect(socket, url_.c_str())) != 0)
                error = url_ + ": " + zmq_strerror(zmq_errno());
        } catch (const std::exception& e) {
            error = e.what();
        }
        ready.set_value(error);
        if (error.empty()) {
            while (!stopping_) {
                if (mode_ == ZMQ_PUB || mode_ == ZMQ_PUSH) {
                    std::shared_ptr<Send> job;
                    {
                        std::unique_lock<std::mutex> l(mutex_);
                        cv_.wait(l, [&] { return stopping_ || !sends_.empty(); });
                        if (stopping_) break;
                        job = sends_.front();
                        sends_.pop_front();
                    }
                    int rc = zmq_send(socket, job->data.data(), job->data.size(), 0);
                    job->done.set_value(rc < 0 ? -1 : 0);
                } else {
                    auto message = std::make_shared<pzmq_data>();
                    if (zmq_msg_recv(message->get(), socket, 0) < 0) continue;
                    if (mode_ == ZMQ_RPC_FUN) {
                        std::string reply;
                        try {
                            if (!zmq_msg_more(message->get())) throw std::runtime_error("missing RPC payload");
                            auto payload = std::make_shared<pzmq_data>();
                            if (zmq_msg_recv(payload->get(), socket, 0) < 0)
                                throw std::runtime_error("RPC receive failed");
                            rpc_callback_fun callback;
                            {
                                std::lock_guard<std::mutex> l(mutex_);
                                callback = actions_.at(message->string());
                            }
                            reply = callback(this, payload);
                        } catch (const std::exception& e) {
                            reply = std::string("RPC_ERROR:") + e.what();
                        }
                        zmq_send(socket, reply.data(), reply.size(), 0);
                    } else {
                        try {
                            if (callback_) callback_(this, message);
                        } catch (const std::exception& e) {
                            fprintf(stderr, "channel callback: %s\n", e.what());
                        }
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (auto& job : sends_) job->done.set_value(-1);
            sends_.clear();
        }
        zmq_close(socket);
        zmq_ctx_term(ctx);
        if (binds && error.empty() && url_.rfind("ipc://", 0) == 0) {
            std::error_code ignored;
            std::filesystem::remove(url_.substr(6), ignored);
        }
    }
};
}  // namespace StackFlows
