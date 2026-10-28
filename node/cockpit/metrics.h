#pragma once
#include "types.h"
#include "json.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace cockpit {
class Histogram {
public:
    void add(Time duration);
    nlohmann::json report() const;

private:
    std::array<uint64_t, 1024> buckets_{};
    uint64_t count_    = 0;
    long double total_ = 0;
    Time max_          = 0;
    Time percentile(double rank) const;
};
class TraceLog {
public:
    explicit TraceLog(const std::string& path);
    ~TraceLog();
    bool write(nlohmann::json record);
    uint64_t dropped() const
    {
        return dropped_.load();
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<nlohmann::json> records_;
    std::thread thread_;
    std::atomic<uint64_t> dropped_{0};
    bool stopping_ = false;
    void run();
};
}  // namespace cockpit
