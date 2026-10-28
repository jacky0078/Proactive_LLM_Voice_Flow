#include "cockpit_task.h"
#include <algorithm>

namespace cockpit {
void CockpitTask::result(const Json& message)
{
    auto object = message.at("object").get<std::string>();
    if (object == "cockpit.link") {
        if (!message.at("data").at("connected").get<bool>()) {
            for (size_t i = 0; i < bindings_.size(); ++i)
                unavailable(i, message.at("data").value("reason", "manager disconnected"));
        }
        return;
    }
    if (object == "cockpit.vehicle.result") {
        const auto& data = message.at("data");
        if (!service_ || service_->instance.service_id != data.at("service_id") ||
            service_->instance.state != ServiceState::Executing)
            return;
        if (now_ns() >= service_->instance.deadline_ns) {
            finish_service(ServiceState::Unknown, "execution result arrived after deadline");
            return;
        }
        if (data.at("phase") == "accepted")
            service_->accepted_ns = data.at("time_ns").get<Time>();
        else if (data.at("success").get<bool>()) {
            service_->completed_ns        = data.at("time_ns").get<Time>();
            service_->execution_succeeded = true;
        } else {
            service_->completed_ns = data.at("time_ns").get<Time>();
            finish_service(ServiceState::Failed, data.value("reason", "vehicle failure"));
        }
        return;
    }
    auto request = message.value("request_id", "");
    auto it =
        std::find_if(calls_.begin(), calls_.end(), [&](const auto& entry) { return entry.second.request == request; });
    if (it == calls_.end()) return;
    auto id    = it->first;
    auto entry = it->second;
    if (message.value("work_id", "") != entry.work) {
        fail_call(id, "response target mismatch");
        return;
    }
    if (now_ns() >= entry.pending.deadline_ns) {
        fail_call(id, "result arrived after call deadline", true);
        return;
    }
    auto& binding = bindings_[size_t(entry.pending.capability)];
    if (entry.pending.capability_generation != binding.generation && entry.kind != CallKind::Cancel) {
        finish_call(id, "obsolete capability result");
        return;
    }
    if (message.value("error", Json::object()).value("code", 0) != 0) {
        fail_call(id, message.at("error").value("message", "model failure"));
        return;
    }
    if (object == "sys.accepted") {
        it->second.accepted_ns = now_ns();
        if (entry.kind == CallKind::Prompt && service_ && service_->instance.service_id == entry.pending.service_id)
            service_->prompt_accepted_ns = now_ns();
        return;
    }
    try {
        const auto& data = message.at("data");
        if (entry.kind == CallKind::Probe) {
            if (object != "sys.probe" || data.value("nonce", "") != request || data.value("protocol_version", 0) < 2)
                throw std::runtime_error("capability protocol probe mismatch");
            if (!data.value("idle", false)) throw std::runtime_error("bound task is still busy");
            if (entry.pending.capability == Capability::Llm && !data.value("request_cancellation", false))
                throw std::runtime_error("LLM lacks request cancellation");
            if (binding.state == CapabilityState::Probing) {
                binding.state = CapabilityState::Ready;
                binding.reason.clear();
                record({{"kind", "capability"},
                        {"work_id", binding.work},
                        {"generation", binding.generation},
                        {"outcome", "READY"},
                        {"latency_ns", now_ns() - binding.probe_started_ns}});
            }
            finish_call(id, "ready");
            complete_binding();
        } else if (entry.kind == CallKind::Cancel) {
            if (object != "llm.cancelled" || data.at("request_id") != entry.text ||
                (data.value("state", "") != "cancelled" && data.value("state", "") != "not_active"))
                throw std::runtime_error("cancellation response mismatch");
            if (binding.generation == entry.pending.capability_generation && binding.work == entry.work) {
                binding.state = CapabilityState::Ready;
                binding.reason.clear();
            }
            auto target = std::find_if(calls_.begin(), calls_.end(),
                                       [&](const auto& item) { return item.second.request == entry.text; });
            if (target != calls_.end()) finish_call(target->first, "cancelled");
            finish_call(id, "cancelled");
        } else if (entry.kind == CallKind::Decision) {
            if (object != "llm.decision.result" || !data.is_object())
                throw std::runtime_error("invalid decision result");
            auto candidate = candidates_.find(entry.pending.candidate_id);
            finish_call(id, "completed");
            if (candidate == candidates_.end()) return;
            if (!context_.valid(candidate->second, now_ns(), true) || now_ns() >= candidate->second.deadline_ns) {
                finish_candidate(candidate->first, "stale decision");
                return;
            }
            auto service = data.at("service").get<std::string>();
            if (service == "no_action" && data.size() == 1) {
                finish_candidate(candidate->first, "no_action");
                return;
            }
            if (service != "air_conditioner" || data.size() != 2 || !data.contains("params") ||
                !data["params"].is_object() || data["params"].size() != 1 || data["params"].at("enabled") != true)
                throw std::runtime_error("decision service or parameters outside whitelist");
            if (context_.signals()[AirConditioner].value > .5) {
                finish_candidate(candidate->first, "service already satisfied");
                return;
            }
            proposals_[candidate->first] = Service::AirConditioner;
        } else if (entry.kind == CallKind::Prompt) {
            if (object != "tts.audio.file") throw std::runtime_error("expected prompt WAV");
            finish_call(id, "completed");
            if (!service_ || service_->instance.service_id != entry.pending.service_id) return;
            if (!session_valid(now_ns()) || service_->instance.session_generation != session_.generation) {
                finish_service(ServiceState::Cancelled, "interaction expired while synthesizing");
                return;
            }
            service_->asked_ns             = now_ns();
            service_->instance.state       = ServiceState::Waiting;
            service_->instance.deadline_ns = now_ns() + confirm_timeout_;
            if (!service_event("WAITING_CONFIRMATION",
                               {{"audio", data}, {"timeout_ms", confirm_timeout_ / millisecond}}))
                finish_service(ServiceState::Failed, "interaction output queue full");
        } else if (entry.kind == CallKind::Feedback || entry.kind == CallKind::UserSpeech) {
            if (object != "asr.utf-8" || !data.is_string()) throw std::runtime_error("expected ASR text");
            finish_call(id, "completed");
            if (entry.kind == CallKind::Feedback)
                feedback_text(data.get<std::string>(), entry.pending.service_id, entry.session_generation,
                              entry.origin);
            else if (question_ && question_->origin.request_id == entry.origin.request_id &&
                     question_->session_generation == entry.session_generation) {
                reply(entry.origin, "asr.utf-8", data);
                question_->text = data.get<std::string>();
            }
        } else if (entry.kind == CallKind::Question) {
            if (object != "llm.utf-8.stream" || data.at("index").get<int>() != entry.expected_index)
                throw std::runtime_error("missing or out-of-order LLM fragment");
            auto delta = data.at("delta").get<std::string>();
            if (entry.text.size() + delta.size() > 4096) throw std::runtime_error("answer exceeds direct TTS limit");
            it->second.text += delta;
            ++it->second.expected_index;
            bool current = question_ && question_->origin.request_id == entry.origin.request_id;
            if (current) reply(entry.origin, object, data);
            if (data.at("finish").get<bool>()) {
                auto text = it->second.text;
                finish_call(id, "completed");
                if (current && !call(Capability::Tts, CallKind::Answer, "tts.utf-8", text, 0, 0, entry.origin))
                    finish_question("failed", "TTS unavailable");
            }
        } else if (entry.kind == CallKind::Answer) {
            if (object != "tts.audio.file") throw std::runtime_error("expected answer WAV");
            finish_call(id, "completed");
            if (question_ && question_->origin.request_id == entry.origin.request_id) {
                reply(entry.origin, object, data);
                finish_question("completed");
            }
        }
    } catch (const std::exception& failure) {
        if (calls_.count(id))
            fail_call(id, failure.what());
        else if (entry.kind == CallKind::Decision)
            finish_candidate(entry.pending.candidate_id, failure.what());
        else
            error(entry.origin, failure.what());
    }
}
}  // namespace cockpit
