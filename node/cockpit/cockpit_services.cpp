#include "cockpit_task.h"
#include <algorithm>
#include <cctype>

namespace cockpit {
namespace {
const char* state_name(ServiceState state)
{
    const char* names[]{"ASKING",   "WAITING_CONFIRMATION", "EXECUTING", "SUCCEEDED", "FAILED", "UNKNOWN", "EXPIRED",
                        "CANCELLED"};
    return names[static_cast<int>(state)];
}
std::string clean_feedback(std::string text)
{
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](unsigned char ch) { return ch < 128 && (std::isspace(ch) || std::ispunct(ch)); }),
               text.end());
    for (const std::string end : {"。", "！", "？"})
        if (text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0)
            text.resize(text.size() - end.size());
    return text;
}
}  // namespace
void CockpitTask::candidate_ready(const Candidate& candidate)
{
    if (candidates_.size() == 16) {
        context_.finish(candidate.need_id, now_ns());
        record({{"kind", "candidate"},
                {"candidate_id", candidate.candidate_id},
                {"outcome", "candidate queue full"},
                {"latency_ns", now_ns() - candidate.observed_ns}});
        return;
    }
    candidates_.emplace(candidate.candidate_id, candidate);
    record({{"kind", "decision"},
            {"candidate_id", candidate.candidate_id},
            {"need", need_name(need_type(candidate.need_id))},
            {"seat", need_seat(candidate.need_id)},
            {"observation_ns", candidate.confirmed_ns - candidate.observed_ns}});
}
void CockpitTask::finish_candidate(uint64_t id, const std::string& outcome, Time cooldown)
{
    auto found = candidates_.find(id);
    if (found == candidates_.end()) return;
    auto candidate = found->second;
    candidates_.erase(found);
    proposals_.erase(id);
    context_.finish(candidate.need_id, now_ns() + cooldown);
    record({{"kind", "candidate"},
            {"candidate_id", id},
            {"seat", need_seat(candidate.need_id)},
            {"need", need_name(need_type(candidate.need_id))},
            {"outcome", outcome},
            {"latency_ns", now_ns() - candidate.observed_ns}});
    std::vector<Call> active;
    for (const auto& entry : calls_)
        if (entry.second.pending.candidate_id == id && entry.second.kind == CallKind::Decision)
            active.push_back(entry.second);
    for (const auto& entry : active) cancel_llm(entry);
}
void CockpitTask::pump()
{
    auto now = now_ns();
    if (question_ && question_->text && !occupied(Capability::Llm) && bindings_[1].state == CapabilityState::Ready) {
        auto text = *question_->text;
        if (call(Capability::Llm, CallKind::Question, "llm.utf-8", text, 0, 0, question_->origin))
            question_->text.reset();
        else
            finish_question("failed", "LLM dispatch unavailable");
    }
    if (paused_ || !session_valid(now)) return;
    const auto& busy = context_.signals()[Busy];
    if (busy.status == Status::Valid && busy.value > 0.5) return;
    // 慢决策只占用模型资源，不提前占用交互资源。
    if (!occupied(Capability::Llm) && bindings_[1].state == CapabilityState::Ready && !question_) {
        for (const auto& entry : candidates_) {
            const auto& candidate = entry.second;
            if (need_type(candidate.need_id) != Need::Comfort || proposals_.count(entry.first)) continue;
            if (candidate.deadline_ns <= now || !context_.valid(candidate, now, true)) continue;
            Json data{{"temperature", candidate.values[0]},
                      {"ac_on", candidate.values[1] > .5},
                      {"occupied", candidate.values[2] > .5},
                      {"speed", candidate.values[3]},
                      {"discomfort_events", 2}};
            call(Capability::Llm, CallKind::Decision, "llm.decision.request", data, entry.first);
            break;
        }
    }
    if (service_ || question_ || occupied(Capability::Tts) || bindings_[2].state != CapabilityState::Ready) return;
    auto policy_started     = now_ns();
    const Candidate* chosen = nullptr;
    int priority            = 4;
    for (const auto& entry : candidates_) {
        const auto& candidate = entry.second;
        auto type             = need_type(candidate.need_id);
        if (candidate.deadline_ns <= now || !context_.valid(candidate, now, type == Need::Comfort)) continue;
        if (type == Need::Comfort && !proposals_.count(entry.first)) continue;
        int rank = type == Need::Rest ? 0 : type == Need::Heat ? 1 : 2;
        if (rank < priority) {
            chosen   = &candidate;
            priority = rank;
        }
    }
    if (chosen) {
        auto type    = need_type(chosen->need_id);
        auto service = type == Need::Rest
                           ? Service::Rest
                           : type == Need::Heat ? Service::AirConditioner : proposals_.at(chosen->candidate_id);
        record({{"kind", "policy"},
                {"candidate_id", chosen->candidate_id},
                {"need", need_name(type)},
                {"latency_ns", now_ns() - policy_started},
                {"outcome", "ask"}});
        ask(*chosen, service);
    }
}
void CockpitTask::ask(const Candidate& candidate, Service type)
{
    ActiveService service;
    service.candidate                   = candidate;
    service.instance.service_id         = ++next_service_;
    service.instance.candidate_id       = candidate.candidate_id;
    service.instance.service_type       = type;
    service.instance.session_generation = session_.generation;
    service.instance.deadline_ns        = now_ns() + model_timeout_;
    service.started_ns                  = now_ns();
    service_                            = service;
    auto text =
        type == Service::AirConditioner ? "车内有些热，需要我帮你打开空调吗？" : "检测到疲劳状态，是否显示休息提醒？";
    if (!call(Capability::Tts, CallKind::Prompt, "tts.utf-8", text, candidate.candidate_id,
              service.instance.service_id)) {
        finish_service(ServiceState::Failed, "TTS dispatch unavailable");
        return;
    }
    if (!service_event("ASKING", {{"text", text}}))
        finish_service(ServiceState::Failed, "interaction output queue full");
}
bool CockpitTask::service_event(const std::string& phase, Json data)
{
    if (!service_ || !session_valid(now_ns()) || service_->instance.session_generation != session_.generation)
        return false;
    data["event_id"]     = ++next_event_;
    data["service_id"]   = service_->instance.service_id;
    data["candidate_id"] = service_->instance.candidate_id;
    data["phase"]        = phase;
    data["service"]      = service_->instance.service_type == Service::AirConditioner ? "air_conditioner" : "rest";
    data["session_generation"] = session_.generation;
    RequestContext event{prefix_ + "event-" + std::to_string(next_event_), session_.reply.reply_url};
    return reply(event, "cockpit.service.event", data);
}
void CockpitTask::finish_service(ServiceState state, const std::string& reason, Time cooldown)
{
    if (!service_) return;
    service_->instance.state = state;
    service_event(state_name(state), {{"reason", reason}});
    auto value = *service_;
    service_.reset();
    auto elapsed = [](Time end, Time begin) -> Json { return end && begin ? Json(end - begin) : Json(nullptr); };
    record({{"kind", "service"},
            {"service_id", value.instance.service_id},
            {"candidate_id", value.instance.candidate_id},
            {"need", need_name(need_type(value.candidate.need_id))},
            {"outcome", state_name(state)},
            {"reason", reason},
            {"latency_ns", now_ns() - value.started_ns},
            {"arbitration_ns", value.started_ns - value.candidate.confirmed_ns},
            {"prompt_acceptance_ns", elapsed(value.prompt_accepted_ns, value.candidate.confirmed_ns)},
            {"prompt_ready_ns", elapsed(value.asked_ns, value.candidate.confirmed_ns)},
            {"user_wait_ns", elapsed(value.feedback_ns, value.asked_ns)},
            {"vehicle_acceptance_ns", elapsed(value.accepted_ns, value.execution_ns)},
            {"vehicle_ack_ns", elapsed(value.completed_ns, value.execution_ns)},
            {"execution_ns", elapsed(value.execution_ns ? now_ns() : 0, value.execution_ns)}});
    finish_candidate(value.instance.candidate_id, state_name(state), cooldown);
}
void CockpitTask::feedback(const Json& data, const RequestContext& origin)
{
    auto action = data.at("action").get<std::string>();
    auto id     = data.at("service_id").get<uint64_t>();
    if (!service_ || service_->instance.service_id != id || service_->instance.state != ServiceState::Waiting)
        throw std::runtime_error("service is not awaiting confirmation");
    if (now_ns() >= service_->instance.deadline_ns) {
        finish_service(ServiceState::Expired, "feedback arrived after confirmation deadline");
        throw std::runtime_error("confirmation deadline exceeded");
    }
    if (action != "confirm" && action != "reject" && action != "cancel")
        throw std::runtime_error("feedback must be confirm, reject or cancel");
    service_->feedback_ns = now_ns();
    if (action == "reject" || action == "cancel") {
        finish_service(ServiceState::Cancelled, action == "reject" ? "user rejected" : "user cancelled",
                       action == "reject" ? reject_cooldown_ : 0);
        reply(origin, "cockpit.feedback", {{"service_id", id}, {"accepted", true}});
        return;
    }
    if (!context_.valid(service_->candidate, now_ns(), need_type(service_->candidate.need_id) == Need::Comfort)) {
        finish_service(ServiceState::Cancelled, "conditions changed before execution");
        throw std::runtime_error("conditions no longer permit execution");
    }
    service_->instance.state       = ServiceState::Executing;
    service_->instance.deadline_ns = now_ns() + execution_timeout_;
    service_->execution_ns         = now_ns();
    if (!vehicle_.execute(service_->instance)) {
        finish_service(ServiceState::Failed, "vehicle command queue full");
        throw std::runtime_error("vehicle command not accepted");
    }
    reply(origin, "cockpit.feedback", {{"service_id", id}, {"accepted", true}});
    service_event("EXECUTING");
}
void CockpitTask::feedback_text(const std::string& raw, uint64_t service, uint64_t session,
                                const RequestContext& origin)
{
    if (!session_valid(now_ns()) || session != session_.generation || !service_ ||
        service_->instance.service_id != service || service_->instance.state != ServiceState::Waiting) {
        error(origin, "speech confirmation no longer belongs to an active service");
        return;
    }
    auto text = clean_feedback(raw);
    std::string action;
    for (const char* yes : {"好", "好的", "可以", "确认", "同意", "打开空调", "开启空调", "yes", "Yes"})
        if (text == yes) action = "confirm";
    for (const char* no : {"不", "不用", "不要", "拒绝", "取消", "no", "No"})
        if (text == no) action = "reject";
    if (action.empty()) {
        reply(origin, "cockpit.feedback",
              {{"service_id", service}, {"accepted", false}, {"text", raw}, {"reason", "ambiguous confirmation"}});
        return;
    }
    feedback({{"service_id", service}, {"action", action}}, origin);
}
void CockpitTask::speech(const Json& data, const RequestContext& origin)
{
    auto path = data.at("path").get<std::string>();
    if (path.empty()) throw std::runtime_error("WAV path required");
    if (service_) {
        if (service_->instance.state != ServiceState::Waiting ||
            data.value("service_id", uint64_t(0)) != service_->instance.service_id)
            throw std::runtime_error("speech must identify the waiting service");
        if (!call(Capability::Asr, CallKind::Feedback, "asr.audio.file", {{"path", path}},
                  service_->instance.candidate_id, service_->instance.service_id, origin))
            throw std::runtime_error("ASR unavailable or busy");
    } else {
        if (question_) throw std::runtime_error("question already active");
        if (bindings_[0].state != CapabilityState::Ready || bindings_[1].state != CapabilityState::Ready ||
            bindings_[2].state != CapabilityState::Ready)
            throw std::runtime_error("ASR, LLM and TTS must be ready for a question");
        question_ = Question{origin, session_.generation, now_ns(), std::nullopt};
        if (!call(Capability::Asr, CallKind::UserSpeech, "asr.audio.file", {{"path", path}}, 0, 0, origin)) {
            question_.reset();
            throw std::runtime_error("ASR unavailable or busy");
        }
    }
}
void CockpitTask::finish_question(const std::string& outcome, const std::string& reason)
{
    if (!question_) return;
    auto question = *question_;
    question_.reset();
    if (!reason.empty()) error(question.origin, reason);
    record({{"kind", "question"},
            {"request_id", question.origin.request_id},
            {"outcome", outcome},
            {"latency_ns", now_ns() - question.started_ns},
            {"reason", reason}});
    std::vector<Call> active;
    for (const auto& entry : calls_)
        if (entry.second.kind == CallKind::Question && entry.second.origin.request_id == question.origin.request_id)
            active.push_back(entry.second);
    for (const auto& entry : active) cancel_llm(entry);
}
void CockpitTask::tick(Time now)
{
    if (!session_.token.empty() && now >= session_.expires) lose_session("lease expired");
    std::vector<uint64_t> expired;
    for (const auto& entry : candidates_) {
        if (service_ && service_->instance.candidate_id == entry.first) continue;
        if (now >= entry.second.deadline_ns ||
            !context_.valid(entry.second, now, need_type(entry.second.need_id) == Need::Comfort))
            expired.push_back(entry.first);
    }
    for (auto id : expired) finish_candidate(id, "expired or evidence changed");
    if (service_) {
        auto& active = *service_;
        if (active.instance.state != ServiceState::Executing &&
            !context_.valid(active.candidate, now, need_type(active.candidate.need_id) == Need::Comfort))
            finish_service(ServiceState::Cancelled, "service evidence no longer valid");
        else if (active.instance.state == ServiceState::Executing && now < active.instance.deadline_ns &&
                 active.execution_succeeded) {
            const auto& ac = context_.signals()[AirConditioner];
            bool readback  = active.instance.service_type != Service::AirConditioner ||
                            (ac.status == Status::Valid && ac.value > .5 && ac.update_time_ns >= active.execution_ns);
            if (readback) finish_service(ServiceState::Succeeded, "execution and readback complete", success_cooldown_);
        }
        if (service_ && now >= service_->instance.deadline_ns)
            finish_service(
                service_->instance.state == ServiceState::Executing ? ServiceState::Unknown : ServiceState::Expired,
                "stage deadline exceeded");
    }
    expired.clear();
    for (auto& entry : calls_) {
        if (now >= entry.second.pending.deadline_ns)
            expired.push_back(entry.first);
        else if (entry.second.retry_ns <= now) {
            models_.send(entry.second.body);
            entry.second.retry_ns = now + 250 * millisecond;
        }
    }
    for (auto id : expired) fail_call(id, "call timeout", true);
    for (size_t i = 0; i < bindings_.size(); ++i) {
        if (bindings_[i].state != CapabilityState::Probing) continue;
        if (now >= bindings_[i].probe_deadline_ns)
            unavailable(i, "capability probe deadline exceeded");
        else
            probe(i);
    }
    complete_binding();
    if (question_ && now - question_->started_ns > 3 * model_timeout_)
        finish_question("expired", "question deadline exceeded");
}
}  // namespace cockpit
