#include "ingress.h"
#include "network/Channel.h"
#include "network/EventLoop.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <cmath>
#include <cstring>
#include <memory>

namespace cockpit {
namespace {
bool behavior(uint16_t id)
{
    return id >= 4 && ((id - 4) % 4 == 1 || (id - 4) % 4 == 3);
}
bool continuous(const SignalUpdate& a, const SignalUpdate& b)
{
    const auto& x = a.sample;
    const auto& y = b.sample;
    if (x.source_id != y.source_id || x.source_generation != y.source_generation || x.quality != y.quality)
        return false;
    if (x.signal_id == Temperature) {
        auto band = [](double value) { return value <= 26 ? 0 : value < 28 ? 1 : 2; };
        return band(x.value) == band(y.value);
    }
    if (x.signal_id == Speed) return (x.value > 0) == (y.value > 0);
    return !behavior(x.signal_id) && x.value == y.value;
}
}  // namespace
Ingress::Ingress(SignalQueue& queue, WakeFd& consumer, std::vector<std::string> paths)
    : queue_(queue), consumer_(consumer), paths_(std::move(paths))
{
    if (paths_.size() > source_count) throw std::runtime_error("too many ingress sockets");
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, bridge_))
        throw std::runtime_error("signal bridge socketpair failed");
    for (auto& fault : faults_) fault.store(0);
}
Ingress::~Ingress()
{
    stop();
    for (auto fd : bridge_)
        if (fd >= 0) close(fd);
}
void Ingress::start()
{
    std::promise<void> ready;
    auto result = ready.get_future();
    thread_     = std::thread([this, ready = std::move(ready)]() mutable { run(std::move(ready)); });
    result.get();
}
void Ingress::stop()
{
    stopping_ = true;
    wake_.notify();
    if (thread_.joinable()) thread_.join();
}
bool Ingress::inject(const SignalFrame& frame)
{
    return send(bridge_[0], &frame, sizeof(frame), MSG_DONTWAIT | MSG_NOSIGNAL) == sizeof(frame);
}
void Ingress::consumed()
{
    if (pending_.load(std::memory_order_acquire)) wake_.notify();
}
void Ingress::lost(const SignalFrame& frame)
{
    ++stats_.overflow;
    auto marker   = (uint64_t(frame.source_generation) << 32) | frame.sequence;
    auto previous = faults_[frame.source_id].load(std::memory_order_relaxed);
    if (marker > previous) faults_[frame.source_id].store(marker, std::memory_order_release);
    consumer_.notify();
}
void Ingress::accept(const SignalFrame& frame)
{
    ++stats_.received;
    if (frame.signal_id >= signal_count || frame.source_id >= source_count || !frame.source_generation ||
        !frame.sequence || frame.quality > 1 || !std::isfinite(frame.value)) {
        ++stats_.malformed;
        return;
    }
    SignalUpdate update{frame, now_ns(), 0};
    auto& previous = last_[frame.signal_id];
    bool same_origin =
        previous.sample.source_id == frame.source_id && previous.sample.source_generation == frame.source_generation;
    if (previous.receive_time_ns && same_origin && frame.sequence <= previous.sample.sequence) {
        ++stats_.filtered;
        return;
    }
    // 同值保留周期刷新；行为事件不按数值去重，避免丢失次数证据。
    if (previous.receive_time_ns && same_origin && !behavior(frame.signal_id) && previous.sample.value == frame.value &&
        previous.sample.quality == frame.quality &&
        update.receive_time_ns - previous.receive_time_ns < 250 * millisecond) {
        ++stats_.filtered;
        return;
    }
    previous = update;
    if (pending_) flush();
    auto& held = cache_[frame.signal_id];
    if (held) {
        if (continuous(*held, update)) {
            held = update;
            ++stats_.coalesced;
        } else {
            lost(frame);
            held.reset();
        }
        wake_.notify();
        return;
    }
    update.enqueue_time_ns = now_ns();
    if (queue_.push(update)) {
        ++stats_.enqueued;
        consumer_.notify();
        return;
    }
    auto& pending = cache_[frame.signal_id];
    if (!behavior(frame.signal_id) && (!pending || continuous(*pending, update))) {
        pending = update;
        pending_.store(true, std::memory_order_release);
        ++stats_.coalesced;
        wake_.notify();
    } else {
        lost(frame);
    }
}
void Ingress::flush()
{
    for (auto& pending : cache_) {
        if (!pending) continue;
        pending->enqueue_time_ns = now_ns();
        if (!queue_.push(*pending)) return;
        pending.reset();
        ++stats_.enqueued;
        consumer_.notify();
    }
    pending_.store(false, std::memory_order_release);
}
void Ingress::receive(int fd)
{
    flush();
    for (size_t count = 0; count < 32; ++count) {
        SignalFrame frame;
        auto size = recv(fd, &frame, sizeof(frame), MSG_DONTWAIT | MSG_TRUNC);
        if (size < 0 && errno == EINTR) {
            --count;
            continue;
        }
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (size != sizeof(frame)) {
            ++stats_.malformed;
            if (size < 0) return;
            continue;
        }
        auto started = now_ns();
        accept(frame);
        auto elapsed = static_cast<uint64_t>(now_ns() - started);
        stats_.processing_ns.fetch_add(elapsed, std::memory_order_relaxed);
        if (elapsed > stats_.processing_max_ns.load(std::memory_order_relaxed))
            stats_.processing_max_ns.store(elapsed, std::memory_order_relaxed);
    }
}
void Ingress::run(std::promise<void> ready)
{
    bool announced = false;
    std::vector<int> sockets;
    std::vector<std::string> owned_paths;
    try {
        network::EventLoop loop;
        std::vector<std::unique_ptr<network::Channel>> channels;
        auto attach = [&](int fd, std::function<void()> callback) {
            auto channel = std::make_unique<network::Channel>(&loop, fd);
            channel->setReadCallback(std::move(callback));
            channel->enableReading();
            channels.push_back(std::move(channel));
        };
        // 注册前先完成可能失败的 bind，避免异常展开时留下活跃 Channel。
        for (const auto& path : paths_) {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if (path.empty() || path.size() >= sizeof(address.sun_path))
                throw std::runtime_error("invalid ingress path");
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) throw std::runtime_error("ingress socket failed");
            sockets.push_back(fd);
            if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
                throw std::runtime_error("cannot bind ingress socket: " + path);
            owned_paths.push_back(path);
        }
        attach(wake_.fd(), [&] {
            wake_.drain();
            if (stopping_)
                loop.quit();
            else
                flush();
        });
        attach(bridge_[1], [&] { receive(bridge_[1]); });
        for (auto fd : sockets) attach(fd, [&, fd] { receive(fd); });
        ready.set_value();
        announced = true;
        loop.loop();
        for (auto& channel : channels) {
            channel->disableAll();
            channel->remove();
        }
    } catch (...) {
        if (!announced)
            ready.set_exception(std::current_exception());
        else
            stopping_ = true;
    }
    for (auto fd : sockets) close(fd);
    for (const auto& path : owned_paths) unlink(path.c_str());
}
}  // namespace cockpit
