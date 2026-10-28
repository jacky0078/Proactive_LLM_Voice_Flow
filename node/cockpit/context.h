#pragma once
#include "timers.h"
#include <functional>

namespace cockpit {
struct ContextConfig {
    Time signal_ttl       = 2 * second;
    Time heat_observe     = 800 * millisecond;
    Time rest_observe     = 2 * second;
    Time behavior_window  = 5 * second;
    Time fast_lifetime    = 5 * second;
    Time slow_lifetime    = 10 * second;
    double hot_threshold  = 28;
    double cool_threshold = 26;
};
class Context {
public:
    Context(ContextConfig config, std::function<void(const Candidate&)> ready);
    bool apply(const SignalUpdate& update);
    void advance(Time now);
    void invalidate_source(uint16_t source, uint32_t generation, uint32_t through_sequence);
    bool valid(const Candidate& candidate, Time now, bool snapshot = false) const;
    void finish(uint16_t need, Time cooldown_until);
    void pause(bool paused);
    Time next_deadline() const
    {
        return timers_.next();
    }
    const std::array<SignalState, signal_count>& signals() const
    {
        return signals_;
    }
    uint64_t updates() const
    {
        return updates_;
    }
    uint64_t rejected() const
    {
        return rejected_;
    }

private:
    struct Evidence {
        Time fanning = 0;
        std::array<Time, 2> discomfort{};
    };
    struct NeedState {
        Time observing        = 0;
        Time cooldown_until   = 0;
        uint64_t candidate_id = 0;
        bool revoked          = false;
    };
    struct Source {
        uint32_t generation   = 0;
        uint32_t lost_through = 0;
        bool known            = false;
    };
    ContextConfig config_;
    std::function<void(const Candidate&)> ready_;
    std::array<SignalState, signal_count> signals_{};
    std::array<uint16_t, signal_count> owners_{};
    std::array<uint32_t, signal_count> sequences_{};
    std::array<Source, source_count> sources_{};
    std::array<Evidence, seat_count> evidence_{};
    std::array<NeedState, need_count> needs_{};
    std::array<uint16_t, signal_count> affected_needs_{};
    Timers timers_;
    uint64_t next_candidate_ = 0, updates_ = 0, rejected_ = 0;
    bool hot_ = false, paused_ = false;
    bool fresh(uint16_t id, Time now) const;
    bool condition(uint16_t need, Time now, bool require_evidence) const;
    void evaluate(Time now);
    void invalidate_needs(uint16_t mask);
    void check_conditions(uint16_t signal, Time now);
    std::array<uint16_t, 4> dependencies(size_t seat) const;
};
}  // namespace cockpit
