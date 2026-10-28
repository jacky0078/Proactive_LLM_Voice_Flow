#include "cockpit_task.h"
#include <algorithm>
#include <cmath>

namespace cockpit {
bool CockpitTask::session_valid(Time now) const
{
    return !session_.token.empty() && now < session_.expires;
}
void CockpitTask::require_session(const Json& data, const RequestContext& context) const
{
    if (!session_valid(now_ns()) || data.value("session_token", "") != session_.token ||
        context.reply_url != session_.reply.reply_url)
        throw std::runtime_error("interaction session invalid or expired");
}
void CockpitTask::lose_session(const std::string& reason)
{
    if (service_ && service_->instance.state != ServiceState::Executing)
        finish_service(ServiceState::Cancelled, reason);
    if (question_) finish_question("cancelled", reason);
    record({{"kind", "session"}, {"outcome", reason}, {"generation", session_.generation}});
    session_.token.clear();
    session_.expires = 0;
}
void CockpitTask::control(const Control& request)
{
    auto object        = request.body.at("object").get<std::string>();
    const auto& data   = request.body.at("data");
    const auto& origin = request.context;
    if (object == "sys.probe") {
        auto result                = data;
        result["perception_ready"] = true;
        reply(origin, "sys.probe", result);
    } else if (object == "cockpit.status")
        reply(origin, object, status());
    else if (object == "cockpit.capabilities.bind")
        bind(data, origin);
    else if (object == "cockpit.session.bind") {
        auto client = data.at("client_id").get<std::string>();
        if (client.empty() || client.size() > 128) throw std::runtime_error("invalid client_id");
        if (session_valid(now_ns()) && session_.client != client) throw std::runtime_error("interaction session busy");
        if (!session_.token.empty()) lose_session("session replaced");
        session_.client  = client;
        session_.reply   = origin;
        session_.token   = prefix_ + "session-" + std::to_string(++session_.generation);
        session_.expires = now_ns() + session_lease_;
        record({{"kind", "session"}, {"outcome", "bound"}, {"generation", session_.generation}});
        reply(origin, object,
              {{"session_token", session_.token},
               {"generation", session_.generation},
               {"lease_ms", session_lease_ / millisecond}});
    } else if (object == "cockpit.session.renew") {
        require_session(data, origin);
        session_.expires = now_ns() + session_lease_;
        reply(origin, object, {{"lease_ms", session_lease_ / millisecond}});
    } else if (object == "cockpit.session.unbind") {
        require_session(data, origin);
        lose_session("unbound");
        reply(origin, object, {{"unbound", true}});
    } else if (object == "cockpit.pause") {
        paused_ = data.value("paused", true);
        context_.pause(paused_);
        reply(origin, "sys.pause", {{"paused", paused_}});
    } else if (object == "cockpit.signal.batch") {
        const auto& updates = data.at("updates");
        if (!updates.is_array() || updates.empty() || updates.size() > 32)
            throw std::runtime_error("signal batch must contain 1..32 updates");
        std::array<SignalFrame, 32> frames;
        size_t count = 0;
        for (const auto& update : updates) {
            auto id     = find_signal(update.at("name").get<std::string>());
            auto source = update.value("source_id", 0);
            if (id < 0 || id == AirConditioner || source < 0 || source >= int(source_count - 1))
                throw std::runtime_error("unknown signal or reserved simulator signal/source");
            auto& frame             = frames[count++];
            frame.signal_id         = id;
            frame.source_id         = source;
            frame.source_generation = update.value("source_generation", 1u);
            frame.sequence          = update.at("sequence").get<uint32_t>();
            frame.value             = update.at("value").get<double>();
            auto quality            = update.value("quality", 1);
            if (quality < 0 || quality > 1 || !frame.source_generation || !frame.sequence ||
                !std::isfinite(frame.value))
                throw std::runtime_error("invalid signal quality, sequence, generation or value");
            frame.quality = static_cast<uint8_t>(quality);
        }
        size_t accepted = 0;
        for (size_t i = 0; i < count && ingress_.inject(frames[i]); ++i) ++accepted;
        reply(origin, object, {{"accepted", accepted}, {"submitted", count}},
              accepted == count ? Json(nullptr)
                                : Json{{"code", -31}, {"message", "signal bridge full; batch partially accepted"}});
    } else if (object == "cockpit.feedback") {
        require_session(data, origin);
        feedback(data, origin);
    } else if (object == "cockpit.speech.file") {
        require_session(data, origin);
        speech(data, origin);
    } else
        throw std::runtime_error("unsupported cockpit object: " + object);
}
void CockpitTask::bind(const Json& data, const RequestContext& origin)
{
    if (data.at("runtime_id") != runtime_id_) throw std::runtime_error("runtime_id mismatch");
    auto version         = data.at("version").get<uint64_t>();
    const auto& snapshot = data.at("bindings");
    const char* names[]{"asr", "llm", "tts"};
    std::array<std::string, 3> work;
    for (size_t i = 0; i < work.size(); ++i) {
        const auto& value = snapshot.at(names[i]);
        if (value.is_null()) continue;
        work[i] = value.get<std::string>();
        if (work[i].rfind(std::string(names[i]) + ".", 0) != 0 || work[i].size() > 64)
            throw std::runtime_error("capability task mismatch");
    }
    if (!version || version < binding_version_ || (version == binding_version_ && snapshot != binding_snapshot_))
        throw std::runtime_error("stale or conflicting capability version");
    if (binding_waiters_.size() == 16) throw std::runtime_error("too many pending bind requests");
    if (version > binding_version_) {
        for (const auto& waiter : binding_waiters_) error(waiter, "capability binding superseded");
        binding_waiters_.clear();
        binding_version_  = version;
        binding_snapshot_ = snapshot;
        for (size_t i = 0; i < work.size(); ++i) {
            auto& binding = bindings_[i];
            if (binding.work == work[i]) continue;
            unavailable(i, "capability binding changed");
            binding.work = work[i];
            ++binding.generation;
            binding.reason.clear();
            binding.state             = work[i].empty() ? CapabilityState::Unbound : CapabilityState::Probing;
            binding.probe_started_ns  = now_ns();
            binding.probe_deadline_ns = binding.probe_started_ns + 3 * second;
            if (!work[i].empty()) probe(i);
        }
    }
    binding_waiters_.push_back(origin);
    complete_binding();
}
void CockpitTask::complete_binding()
{
    for (const auto& binding : bindings_)
        if (binding.state == CapabilityState::Probing) return;
    if (binding_waiters_.empty()) return;
    auto data = Json{{"version", binding_version_}, {"capabilities", status()["capabilities"]}};
    for (const auto& waiter : binding_waiters_) reply(waiter, "cockpit.capabilities.bound", data);
    binding_waiters_.clear();
}
void CockpitTask::probe(size_t capability)
{
    auto& binding = bindings_[capability];
    if (binding.state != CapabilityState::Probing || !models_.connected()) return;
    for (const auto& entry : calls_)
        if (entry.second.kind == CallKind::Probe && size_t(entry.second.pending.capability) == capability) return;
    call(static_cast<Capability>(capability), CallKind::Probe, "sys.probe", Json::object());
}
void CockpitTask::unavailable(size_t capability, const std::string& reason)
{
    auto& binding  = bindings_[capability];
    binding.state  = binding.work.empty() ? CapabilityState::Unbound : CapabilityState::Unavailable;
    binding.reason = reason;
    std::vector<uint64_t> affected;
    for (const auto& entry : calls_)
        if (size_t(entry.second.pending.capability) == capability && entry.second.kind != CallKind::Cancel)
            affected.push_back(entry.first);
    for (auto id : affected) {
        auto it = calls_.find(id);
        if (it != calls_.end()) cancel_llm(it->second);
        fail_call(id, reason);
    }
}
bool CockpitTask::occupied(Capability capability) const
{
    for (const auto& entry : calls_)
        if (entry.second.pending.capability == capability && entry.second.kind != CallKind::Probe) return true;
    return false;
}
uint64_t CockpitTask::call(Capability capability, CallKind kind, const std::string& object, const Json& data,
                           uint64_t candidate, uint64_t service, RequestContext origin)
{
    auto& binding = bindings_[size_t(capability)];
    if (calls_.size() >= 32 ||
        (kind != CallKind::Probe && (binding.state != CapabilityState::Ready || occupied(capability))))
        return 0;
    Call entry;
    entry.pending = {++next_call_, candidate, service, capability, binding.generation, 0, now_ns()};
    Time timeout  = kind == CallKind::Probe ? 2 * second : kind == CallKind::Decision ? llm_timeout_ : model_timeout_;
    entry.pending.deadline_ns = now_ns() + timeout;
    if (candidate && candidates_.count(candidate) && kind == CallKind::Decision)
        entry.pending.deadline_ns = std::min(entry.pending.deadline_ns, candidates_.at(candidate).deadline_ns);
    entry.kind               = kind;
    entry.work               = binding.work;
    entry.request            = prefix_ + std::to_string(entry.pending.call_id);
    entry.origin             = std::move(origin);
    entry.session_generation = session_.generation;
    entry.body               = {{"request_id", entry.request},
                  {"work_id", entry.work},
                  {"action", "inference"},
                  {"object", object},
                  {"data", data},
                  {"ack_requested", kind != CallKind::Probe}};
    if (kind == CallKind::Probe) {
        entry.body["data"]["nonce"] = entry.request;
        entry.retry_ns              = now_ns() + 250 * millisecond;
    }
    if (!models_.send(entry.body)) {
        record({{"kind", "call"},
                {"call_id", entry.pending.call_id},
                {"candidate_id", candidate},
                {"service_id", service},
                {"request_id", entry.request},
                {"work_id", entry.work},
                {"outcome", "dispatch unavailable"},
                {"latency_ns", now_ns() - entry.pending.started_ns},
                {"acceptance_ns", nullptr}});
        return 0;
    }
    auto id = entry.pending.call_id;
    calls_.emplace(id, std::move(entry));
    return id;
}
void CockpitTask::finish_call(uint64_t id, const std::string& outcome)
{
    auto it = calls_.find(id);
    if (it == calls_.end()) return;
    const auto& call = it->second;
    record({{"kind", "call"},
            {"call_id", id},
            {"candidate_id", call.pending.candidate_id},
            {"service_id", call.pending.service_id},
            {"request_id", call.request},
            {"work_id", call.work},
            {"operation", static_cast<int>(call.kind)},
            {"outcome", outcome},
            {"latency_ns", now_ns() - call.pending.started_ns},
            {"acceptance_ns", call.accepted_ns ? Json(call.accepted_ns - call.pending.started_ns) : Json(nullptr)}});
    calls_.erase(it);
}
void CockpitTask::cancel_llm(const Call& target)
{
    if (target.pending.capability != Capability::Llm || target.kind == CallKind::Probe ||
        target.kind == CallKind::Cancel)
        return;
    for (const auto& entry : calls_)
        if (entry.second.kind == CallKind::Cancel && entry.second.text == target.request) return;
    Call cancel;
    cancel.pending = {++next_call_,    target.pending.candidate_id,          target.pending.service_id,
                      Capability::Llm, target.pending.capability_generation, now_ns() + 2 * second,
                      now_ns()};
    cancel.kind    = CallKind::Cancel;
    cancel.work    = target.work;
    cancel.request = prefix_ + std::to_string(cancel.pending.call_id);
    cancel.text    = target.request;
    cancel.body    = {
        {"request_id", cancel.request},
        {"work_id", target.work},
        {"action", "inference"},
        {"object", "llm.cancel"},
        {"data", {{"request_id", target.request}, {"capability_generation", target.pending.capability_generation}}}};
    auto& binding = bindings_[size_t(Capability::Llm)];
    if (binding.generation == target.pending.capability_generation) {
        binding.state  = CapabilityState::Unavailable;
        binding.reason = "awaiting request cancellation";
    }
    if (models_.send(cancel.body)) calls_.emplace(cancel.pending.call_id, std::move(cancel));
}
void CockpitTask::fail_call(uint64_t id, const std::string& reason, bool timed_out)
{
    auto it = calls_.find(id);
    if (it == calls_.end()) return;
    auto entry = it->second;
    if (timed_out && entry.kind != CallKind::Probe && entry.kind != CallKind::Cancel) {
        auto& binding = bindings_[size_t(entry.pending.capability)];
        if (binding.generation == entry.pending.capability_generation) {
            binding.state  = CapabilityState::Unavailable;
            binding.reason = reason;
        }
        cancel_llm(entry);
    }
    finish_call(id, reason);
    if (entry.kind == CallKind::Probe) {
        auto& binding = bindings_[size_t(entry.pending.capability)];
        if (binding.generation == entry.pending.capability_generation) {
            binding.state  = CapabilityState::Unavailable;
            binding.reason = reason;
        }
        complete_binding();
    } else if (entry.kind == CallKind::Cancel) {
        auto& binding = bindings_[size_t(Capability::Llm)];
        if (binding.generation == entry.pending.capability_generation) {
            binding.state  = CapabilityState::Unavailable;
            binding.reason = "cancellation unconfirmed: " + reason;
        }
    } else if (entry.kind == CallKind::Decision)
        finish_candidate(entry.pending.candidate_id, reason);
    else if (entry.kind == CallKind::Prompt && service_ && service_->instance.service_id == entry.pending.service_id)
        finish_service(ServiceState::Failed, reason);
    else if (entry.kind == CallKind::Feedback)
        error(entry.origin, reason);
    else if (question_ && question_->origin.request_id == entry.origin.request_id)
        finish_question("failed", reason);
}
}  // namespace cockpit
