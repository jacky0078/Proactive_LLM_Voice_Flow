#pragma once
#include "context.h"
#include "ingress.h"
#include "metrics.h"
#include "model_client.h"
#include "vehicle_simulator.h"
#include "channel.h"
#include <deque>
#include <map>
#include <optional>

namespace cockpit {
using Json           = nlohmann::json;
using RequestContext = StackFlows::RequestContext;
class CockpitTask {
public:
    CockpitTask(std::shared_ptr<StackFlows::llm_channel_obj> channel, const Json& config);
    ~CockpitTask();
    void start();
    void stop();
    void submit(const Json& body, const RequestContext& context);

private:
    enum class CapabilityState { Unbound, Probing, Ready, Unavailable };
    enum class CallKind { Probe, Decision, Prompt, Feedback, UserSpeech, Question, Answer, Cancel };
    struct Binding {
        std::string work, reason;
        uint32_t generation    = 0;
        CapabilityState state  = CapabilityState::Unbound;
        Time probe_deadline_ns = 0, probe_started_ns = 0;
    };
    struct Call {
        PendingCall pending;
        CallKind kind = CallKind::Probe;
        std::string work, request, text;
        RequestContext origin;
        Time retry_ns = never, accepted_ns = 0;
        uint64_t session_generation = 0;
        int expected_index          = 0;
        Json body;
    };
    struct Session {
        std::string client, token;
        RequestContext reply;
        uint64_t generation = 0;
        Time expires        = 0;
    };
    struct ActiveService {
        ServiceInstance instance;
        Candidate candidate;
        Time started_ns = 0, asked_ns = 0, execution_ns = 0, accepted_ns = 0, prompt_accepted_ns = 0;
        Time feedback_ns = 0, completed_ns = 0;
        bool execution_succeeded = false;
    };
    struct Question {
        RequestContext origin;
        uint64_t session_generation = 0;
        Time started_ns             = 0;
        std::optional<std::string> text;
    };
    struct Control {
        Json body;
        RequestContext context;
    };
    struct Output {
        RequestContext context;
        std::string object;
        Json data, error;
    };
    std::shared_ptr<StackFlows::llm_channel_obj> channel_;
    Json config_;
    std::string runtime_id_, prefix_;
    WakeFd wake_, output_wake_;
    SignalQueue signals_;
    Ingress ingress_;
    TraceLog trace_;
    Context context_;
    ModelClient models_;
    VehicleSimulator vehicle_;
    Mailbox<Control> controls_;
    Mailbox<Json> results_;
    Mailbox<Output> outputs_;
    std::thread core_, publisher_;
    std::atomic<bool> stopping_{false}, output_stopped_{false}, result_overflow_{false};
    std::atomic<uint64_t> output_dropped_{0};
    std::array<uint64_t, source_count> faults_{};
    std::array<Histogram, 4> signal_latency_;
    std::array<Histogram, source_count> source_latency_;
    std::array<uint64_t, source_count> source_updates_{};
    std::array<Binding, 3> bindings_{};
    std::map<uint64_t, Candidate> candidates_;
    std::map<uint64_t, Service> proposals_;
    std::map<uint64_t, Call> calls_;
    std::optional<ActiveService> service_;
    std::optional<Question> question_;
    Session session_;
    uint64_t next_call_ = 0, next_service_ = 0, next_event_ = 0, binding_version_ = 0;
    Json binding_snapshot_;
    std::vector<RequestContext> binding_waiters_;
    Time session_lease_, confirm_timeout_, execution_timeout_, llm_timeout_, model_timeout_, started_ns_;
    Time success_cooldown_, reject_cooldown_;
    size_t prefix_remaining_  = 0;
    uint64_t max_queue_depth_ = 0;
    bool paused_              = false;

    void run(std::promise<void> ready);
    void publish();
    bool receive(Json message);
    bool reply(const RequestContext&, const std::string&, const Json&, const Json& error = nullptr);
    void error(const RequestContext&, const std::string& reason);
    void control(const Control& request);
    void result(const Json& message);
    void tick(Time now);
    Time next_deadline(Time now) const;
    Json status() const;
    void record(Json data);
    void candidate_ready(const Candidate& candidate);
    void finish_candidate(uint64_t id, const std::string& outcome, Time cooldown = 0);
    bool session_valid(Time now) const;
    void require_session(const Json& data, const RequestContext& context) const;
    void lose_session(const std::string& reason);
    void bind(const Json& data, const RequestContext& context);
    void complete_binding();
    void probe(size_t capability);
    void unavailable(size_t capability, const std::string& reason);
    uint64_t call(Capability capability, CallKind kind, const std::string& object, const Json& data,
                  uint64_t candidate = 0, uint64_t service = 0, RequestContext origin = {});
    void finish_call(uint64_t id, const std::string& outcome);
    void fail_call(uint64_t id, const std::string& reason, bool timed_out = false);
    void cancel_llm(const Call& target);
    bool occupied(Capability capability) const;
    void pump();
    void ask(const Candidate& candidate, Service type);
    bool service_event(const std::string& phase, Json data = Json::object());
    void finish_service(ServiceState state, const std::string& reason, Time cooldown = 0);
    void feedback(const Json& data, const RequestContext& origin);
    void feedback_text(const std::string& text, uint64_t service, uint64_t session, const RequestContext& origin);
    void speech(const Json& data, const RequestContext& origin);
    void finish_question(const std::string& outcome, const std::string& reason = "");
};
}  // namespace cockpit
