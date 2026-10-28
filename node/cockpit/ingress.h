#pragma once
#include "mailbox.h"
#include "types.h"
#include <boost/lockfree/spsc_queue.hpp>
#include <functional>
#include <future>
#include <thread>
#include <vector>

namespace cockpit {
using SignalQueue = boost::lockfree::spsc_queue<SignalUpdate, boost::lockfree::capacity<1024>>;
struct IngressStats {
    std::atomic<uint64_t> received{0}, enqueued{0}, filtered{0}, coalesced{0}, overflow{0}, malformed{0};
    std::atomic<uint64_t> processing_ns{0}, processing_max_ns{0};
};
class Ingress {
public:
    Ingress(SignalQueue& queue, WakeFd& consumer, std::vector<std::string> paths);
    ~Ingress();
    void start();
    void stop();
    bool inject(const SignalFrame& frame);
    void consumed();
    uint64_t fault(size_t source) const
    {
        return faults_[source].load(std::memory_order_acquire);
    }
    const IngressStats& stats() const
    {
        return stats_;
    }

private:
    SignalQueue& queue_;
    WakeFd& consumer_;
    std::vector<std::string> paths_;
    WakeFd wake_;
    int bridge_[2]{-1, -1};
    std::thread thread_;
    std::atomic<bool> stopping_{false}, pending_{false};
    std::array<std::atomic<uint64_t>, source_count> faults_{};
    std::array<SignalUpdate, signal_count> last_{};
    std::array<std::optional<SignalUpdate>, signal_count> cache_{};
    IngressStats stats_;
    void run(std::promise<void> ready);
    void receive(int fd);
    void accept(const SignalFrame& frame);
    void flush();
    void lost(const SignalFrame& frame);
};
}  // namespace cockpit
