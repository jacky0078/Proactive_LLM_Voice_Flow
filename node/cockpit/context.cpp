#include "context.h"
#include <cmath>

namespace cockpit {
namespace {
constexpr const char* names[] = {"cabin.temperature",       "hvac.on",
                                 "vehicle.speed",           "interaction.busy",
                                 "driver.present",          "driver.fanning",
                                 "driver.fatigue",          "driver.discomfort",
                                 "front_passenger.present", "front_passenger.fanning",
                                 "front_passenger.fatigue", "front_passenger.discomfort",
                                 "rear_left.present",       "rear_left.fanning",
                                 "rear_left.fatigue",       "rear_left.discomfort",
                                 "rear_right.present",      "rear_right.fanning",
                                 "rear_right.fatigue",      "rear_right.discomfort"};
}
const char* signal_name(size_t id)
{
    return id < signal_count ? names[id] : "unknown";
}
int find_signal(const std::string& name)
{
    for (size_t i = 0; i < signal_count; ++i)
        if (name == names[i]) return static_cast<int>(i);
    return -1;
}
const char* need_name(Need need)
{
    switch (need) {
        case Need::Heat:
            return "heat";
        case Need::Rest:
            return "rest";
        case Need::Comfort:
            return "comfort";
    }
    return "unknown";
}
Context::Context(ContextConfig config, std::function<void(const Candidate&)> ready)
    : config_(config), ready_(std::move(ready))
{
    if (config.signal_ttl <= 0 || config.heat_observe <= 0 || config.rest_observe <= 0 || config.behavior_window <= 0 ||
        config.fast_lifetime <= 0 || config.slow_lifetime <= 0 || config.cool_threshold >= config.hot_threshold)
        throw std::runtime_error("invalid context configuration");
    owners_.fill(source_count);
    // 固定反向索引：一次更新只检查依赖该信号的少数需求。
    for (size_t seat = 0; seat < seat_count; ++seat) {
        for (auto signal : {Temperature, AirConditioner, Occupied, Fanning})
            affected_needs_[signal_id(signal, seat)] |= uint16_t(1u << (seat * 3));
        for (auto signal : {Speed, Occupied, Fatigue})
            affected_needs_[signal_id(signal, seat)] |= uint16_t(1u << (seat * 3 + 1));
        for (auto signal : {Temperature, AirConditioner, Speed, Occupied, Discomfort, Fanning, Fatigue})
            affected_needs_[signal_id(signal, seat)] |= uint16_t(1u << (seat * 3 + 2));
    }
}
bool Context::fresh(uint16_t id, Time now) const
{
    const auto& state = signals_[id];
    return state.status == Status::Valid && now >= state.update_time_ns &&
           now - state.update_time_ns < config_.signal_ttl;
}
void Context::invalidate_source(uint16_t source, uint32_t generation, uint32_t through_sequence)
{
    if (source >= source_count) return;
    auto& origin = sources_[source];
    if (origin.known && generation < origin.generation) return;
    if (!origin.known || generation > origin.generation) origin = {generation, 0, true};
    origin.lost_through = std::max(origin.lost_through, through_sequence);
    uint16_t affected   = 0;
    for (size_t i = 0; i < signal_count; ++i) {
        if (owners_[i] != source) continue;
        affected |= affected_needs_[i];
        if (i == Temperature) hot_ = false;
        if (i >= 4) evidence_[(i - 4) / 4] = {};
        signals_[i].status = Status::Invalid;
        ++signals_[i].version;
        sequences_[i] = 0;
        timers_.erase(i);
    }
    invalidate_needs(affected);
}
void Context::invalidate_needs(uint16_t mask)
{
    for (size_t id = 0; id < need_count; ++id) {
        if (!(mask & (1u << id))) continue;
        needs_[id].observing = 0;
        if (needs_[id].candidate_id) needs_[id].revoked = true;
        timers_.erase(signal_count + id);
    }
}
void Context::check_conditions(uint16_t signal, Time now)
{
    auto mask = affected_needs_[signal];
    for (size_t id = 0; id < need_count; ++id) {
        if (!(mask & (1u << id))) continue;
        auto& need = needs_[id];
        // 即使解除和恢复出现在同一批，持续观察也必须重新计时，旧候选不能复活。
        if (!condition(id, now, true)) {
            need.observing = 0;
            timers_.erase(signal_count + id);
        }
        if (need.candidate_id && !condition(id, now, false)) need.revoked = true;
    }
}
bool Context::apply(const SignalUpdate& update)
{
    const auto& sample = update.sample;
    if (sample.signal_id >= signal_count || sample.source_id >= source_count || !std::isfinite(sample.value) ||
        !sample.source_generation || !sample.sequence) {
        ++rejected_;
        return false;
    }
    auto& source = sources_[sample.source_id];
    if (!source.known || sample.source_generation > source.generation) {
        invalidate_source(sample.source_id, sample.source_generation, 0);
    }
    auto id = sample.signal_id;
    if (sample.source_generation != source.generation || sample.sequence <= source.lost_through ||
        (owners_[id] == sample.source_id && sample.sequence <= sequences_[id]) ||
        (owners_[id] != source_count && owners_[id] != sample.source_id)) {
        ++rejected_;
        return false;
    }
    owners_[id]    = sample.source_id;
    sequences_[id] = sample.sequence;
    auto& state    = signals_[id];
    if (state.status == Status::Valid && update.receive_time_ns >= state.update_time_ns + config_.signal_ttl) {
        invalidate_needs(affected_needs_[id]);
        if (id == Temperature) hot_ = false;
        if (id >= 4) evidence_[(id - 4) / 4] = {};
        ++state.version;
    }
    auto status = sample.quality ? Status::Valid : Status::Invalid;
    if (state.value != sample.value || state.status != status) ++state.version;
    state.value          = sample.value;
    state.status         = status;
    state.update_time_ns = update.receive_time_ns;
    timers_.set(id, status == Status::Valid ? update.receive_time_ns + config_.signal_ttl : never);
    if (id == Temperature && status == Status::Valid) {
        if (sample.value >= config_.hot_threshold) hot_ = true;
        if (sample.value <= config_.cool_threshold) hot_ = false;
    }
    if (status != Status::Valid) {
        invalidate_needs(affected_needs_[id]);
        if (id == Temperature) hot_ = false;
        if (id >= 4) evidence_[(id - 4) / 4] = {};
    }
    if (id >= 4 && (id - 4) % 4 == 0 && sample.value < .5) evidence_[(id - 4) / 4] = {};
    if (id >= 4 && status == Status::Valid) {
        auto seat = (id - 4) / 4;
        auto kind = (id - 4) % 4;
        if (kind == 1 && sample.value >= 0.8) evidence_[seat].fanning = update.receive_time_ns;
        if (kind == 3 && sample.value >= 0.8) {
            auto& times = evidence_[seat].discomfort;
            times[0]    = times[1];
            times[1]    = update.receive_time_ns;
        }
    }
    check_conditions(id, update.receive_time_ns);
    ++updates_;
    return true;
}
std::array<uint16_t, 4> Context::dependencies(size_t seat) const
{
    return {Temperature, AirConditioner, signal_id(Occupied, seat), Speed};
}
bool Context::condition(uint16_t need, Time now, bool require_evidence) const
{
    auto seat = need_seat(need);
    if (!fresh(signal_id(Occupied, seat), now) || signals_[signal_id(Occupied, seat)].value < 0.5) return false;
    if (need_type(need) == Need::Rest) {
        return seat == 0 && fresh(Speed, now) && signals_[Speed].value > 0 && fresh(signal_id(Fatigue, seat), now) &&
               signals_[signal_id(Fatigue, seat)].value >= 0.8;
    }
    if (!fresh(Temperature, now) || !fresh(AirConditioner, now)) return false;
    if (need_type(need) == Need::Heat) {
        return hot_ && signals_[AirConditioner].value < 0.5 &&
               (!require_evidence ||
                (evidence_[seat].fanning && now - evidence_[seat].fanning < config_.behavior_window));
    }
    if (!fresh(Speed, now) || condition(seat * 3, now, true) || condition(seat * 3 + 1, now, true)) return false;
    return !require_evidence ||
           (evidence_[seat].discomfort[0] && now - evidence_[seat].discomfort[0] < config_.behavior_window);
}
bool Context::valid(const Candidate& candidate, Time now, bool snapshot) const
{
    if (candidate.need_id >= need_count) return false;
    const auto& need = needs_[candidate.need_id];
    if (need.candidate_id != candidate.candidate_id || need.revoked || !condition(candidate.need_id, now, false))
        return false;
    if (snapshot && need_type(candidate.need_id) == Need::Comfort) {
        auto ids = dependencies(need_seat(candidate.need_id));
        for (size_t i = 0; i < ids.size(); ++i)
            if (!fresh(ids[i], now) || signals_[ids[i]].version != candidate.versions[i]) return false;
    }
    return true;
}
void Context::evaluate(Time now)
{
    for (size_t id = 0; id < need_count; ++id) {
        auto& state = needs_[id];
        if (paused_ || state.candidate_id || now < state.cooldown_until) continue;
        if (!condition(id, now, true)) {
            state.observing = 0;
            timers_.erase(signal_count + id);
            continue;
        }
        if (!state.observing) state.observing = now;
        Time wait =
            need_type(id) == Need::Heat ? config_.heat_observe : need_type(id) == Need::Rest ? config_.rest_observe : 0;
        if (now < state.observing + wait) {
            timers_.set(signal_count + id, state.observing + wait);
            continue;
        }
        Candidate candidate;
        candidate.candidate_id = ++next_candidate_;
        candidate.need_id      = id;
        candidate.observed_ns  = state.observing;
        candidate.confirmed_ns = now;
        candidate.deadline_ns  = now + (need_type(id) == Need::Comfort ? config_.slow_lifetime : config_.fast_lifetime);
        if (need_type(id) == Need::Comfort) {
            auto ids = dependencies(need_seat(id));
            for (size_t i = 0; i < ids.size(); ++i) {
                candidate.values[i]   = signals_[ids[i]].value;
                candidate.versions[i] = signals_[ids[i]].version;
            }
        }
        state.candidate_id = candidate.candidate_id;
        state.revoked      = false;
        timers_.erase(signal_count + id);
        ready_(candidate);
    }
}
void Context::advance(Time now)
{
    uint32_t id;
    while (timers_.pop(now, id)) {
        if (id < signal_count && signals_[id].status == Status::Valid) {
            signals_[id].status = Status::Stale;
            ++signals_[id].version;
            check_conditions(id, now);
            if (id == Temperature) hot_ = false;
        }
    }
    evaluate(now);
}
void Context::finish(uint16_t need, Time cooldown_until)
{
    if (need >= need_count) return;
    needs_[need]               = {0, cooldown_until, 0, false};
    evidence_[need_seat(need)] = {};
    timers_.erase(signal_count + need);
}
void Context::pause(bool paused)
{
    paused_ = paused;
    if (paused) {
        for (size_t id = 0; id < need_count; ++id) {
            needs_[id].observing = 0;
            timers_.erase(signal_count + id);
        }
    }
}
}  // namespace cockpit
