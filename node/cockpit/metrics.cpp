#include "metrics.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace cockpit {
void Histogram::add(Time duration)
{
    auto value    = static_cast<uint64_t>(std::max<Time>(0, duration));
    size_t bucket = value;
    if (value >= 16) {
        unsigned exponent = 63 - __builtin_clzll(value) - 4;
        bucket            = exponent * 16 + (value >> exponent);
    }
    ++buckets_[std::min(bucket, buckets_.size() - 1)];
    ++count_;
    total_ += value;
    max_ = std::max(max_, duration);
}
Time Histogram::percentile(double rank) const
{
    auto target   = static_cast<uint64_t>(std::ceil(count_ * rank));
    uint64_t seen = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
        seen += buckets_[i];
        if (!target || seen < target) continue;
        if (i < 16) return std::min<Time>(max_, i);
        unsigned exponent = i / 16 - 1;
        long double upper = std::ldexp(static_cast<long double>(17 + i % 16), exponent) - 1;
        return upper >= max_ ? max_ : static_cast<Time>(upper);
    }
    return max_;
}
nlohmann::json Histogram::report() const
{
    using Json = nlohmann::json;
    auto ms    = [](Time time) { return time / 1e6; };
    return {{"count", count_},
            {"mean_ms", count_ ? Json(double(total_ / count_ / 1e6)) : Json(nullptr)},
            {"p50_ms", count_ ? Json(ms(percentile(.5))) : Json(nullptr)},
            {"p95_ms", count_ ? Json(ms(percentile(.95))) : Json(nullptr)},
            {"p99_ms", count_ ? Json(ms(percentile(.99))) : Json(nullptr)},
            {"p999_ms", count_ >= 1000 ? Json(ms(percentile(.999))) : Json(nullptr)},
            {"max_ms", count_ ? Json(ms(max_)) : Json(nullptr)},
            {"quantiles", "bounded_histogram_upper_estimates"}};
}
TraceLog::TraceLog(const std::string& path) : file_(path, std::ios::app)
{
    if (!file_) throw std::runtime_error("cannot open cockpit trace log: " + path);
    thread_ = std::thread([this] { run(); });
}
TraceLog::~TraceLog()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}
bool TraceLog::write(nlohmann::json record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.size() == 512) {
        ++dropped_;
        return false;
    }
    records_.push_back(std::move(record));
    cv_.notify_one();
    return true;
}
void TraceLog::run()
{
    for (;;) {
        nlohmann::json record;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !records_.empty(); });
            if (records_.empty()) break;
            record = std::move(records_.front());
            records_.pop_front();
        }
        file_ << record.dump() << '\n';
        file_.flush();
        if (!file_) ++dropped_;
    }
}
}  // namespace cockpit
