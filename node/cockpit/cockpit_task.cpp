#include "cockpit_task.h"
#include "network/Channel.h"
#include "network/EventLoop.h"
#include <sys/timerfd.h>
#include <poll.h>
#include <filesystem>
#include <unistd.h>

namespace cockpit {
namespace {
Time duration(const Json& config, const char* key, int fallback)
{
    auto value = config.value(key, fallback);
    if (value <= 0 || value > 3600000) throw std::runtime_error(std::string("invalid duration: ") + key);
    return value * millisecond;
}
ContextConfig context_config(const Json& config)
{
    ContextConfig value;
    value.signal_ttl      = duration(config, "signal_ttl_ms", 2000);
    value.heat_observe    = duration(config, "heat_observe_ms", 800);
    value.rest_observe    = duration(config, "rest_observe_ms", 2000);
    value.behavior_window = duration(config, "behavior_window_ms", 5000);
    value.fast_lifetime   = duration(config, "fast_lifetime_ms", 5000);
    value.slow_lifetime   = duration(config, "slow_lifetime_ms", 10000);
    return value;
}
std::string trace_path(const Json& config)
{
    auto path = std::filesystem::absolute(config.at("output_dir").get<std::string>());
    std::filesystem::create_directories(path);
    return (path / "traces.jsonl").string();
}
}  // namespace
CockpitTask::CockpitTask(std::shared_ptr<StackFlows::llm_channel_obj> channel, const Json& config)
    : channel_(std::move(channel)),
      config_(config),
      runtime_id_(config.at("runtime_id").get<std::string>()),
      prefix_(runtime_id_ + "-" + std::to_string(now_ns()) + "-"),
      ingress_(signals_, wake_, config.value("signal_sockets", std::vector<std::string>{})),
      trace_(trace_path(config)),
      context_(context_config(config), [this](const Candidate& candidate) { candidate_ready(candidate); }),
      models_(config.value("manager_host", "127.0.0.1"), config.value("manager_port", 10001),
              [this](Json message) { return receive(std::move(message)); }),
      vehicle_(
          ingress_, [this](Json message) { return receive(std::move(message)); },
          config.value("vehicle", Json::object())),
      session_lease_(duration(config, "session_lease_ms", 6000)),
      confirm_timeout_(duration(config, "confirmation_timeout_ms", 30000)),
      execution_timeout_(duration(config, "execution_timeout_ms", 3000)),
      llm_timeout_(duration(config, "llm_timeout_ms", 8000)),
      model_timeout_(duration(config, "model_timeout_ms", 120000)),
      started_ns_(now_ns()),
      success_cooldown_(duration(config, "success_cooldown_ms", 300000)),
      reject_cooldown_(duration(config, "reject_cooldown_ms", 600000))
{
    if (runtime_id_.empty() || runtime_id_.size() > 128) throw std::runtime_error("invalid runtime_id");
}
CockpitTask::~CockpitTask()
{
    stop();
}
void CockpitTask::start()
{
    publisher_ = std::thread([this] { publish(); });
    try {
        ingress_.start();
        vehicle_.start();
        models_.start();
        std::promise<void> ready;
        auto result = ready.get_future();
        core_       = std::thread([this, ready = std::move(ready)]() mutable { run(std::move(ready)); });
        result.get();
    } catch (...) {
        stop();
        throw;
    }
}
void CockpitTask::stop()
{
    if (stopping_.exchange(true)) return;
    wake_.notify();
    if (core_.joinable()) core_.join();
    models_.stop();
    vehicle_.stop();
    ingress_.stop();
    output_stopped_ = true;
    output_wake_.notify();
    if (publisher_.joinable()) publisher_.join();
}
bool CockpitTask::receive(Json message)
{
    bool accepted = results_.push(std::move(message));
    if (!accepted) result_overflow_ = true;
    wake_.notify();
    return accepted;
}
void CockpitTask::submit(const Json& body, const RequestContext& context)
{
    if (stopping_ || !controls_.push({body, context})) {
        channel_->send_with_context(context, "sys.error", nullptr,
                                    {{"code", -30}, {"message", "cockpit control queue full or stopping"}});
        return;
    }
    wake_.notify();
}
bool CockpitTask::reply(const RequestContext& context, const std::string& object, const Json& data, const Json& failure)
{
    if (!outputs_.push({context, object, data, failure})) {
        ++output_dropped_;
        return false;
    }
    output_wake_.notify();
    return true;
}
void CockpitTask::error(const RequestContext& context, const std::string& reason)
{
    reply(context, "sys.error", nullptr, {{"code", -30}, {"message", reason}});
}
void CockpitTask::publish()
{
    for (;;) {
        Output output;
        while (outputs_.pop(output)) {
            try {
                if (channel_->send_with_context(output.context, output.object, output.data, output.error))
                    ++output_dropped_;
            } catch (...) {
                ++output_dropped_;
            }
        }
        if (output_stopped_) return;
        pollfd event{output_wake_.fd(), POLLIN, 0};
        if (poll(&event, 1, -1) > 0) output_wake_.drain();
    }
}
void CockpitTask::record(Json data)
{
    data["runtime_id"] = runtime_id_;
    data["time_ns"]    = now_ns();
    trace_.write(std::move(data));
}
void CockpitTask::run(std::promise<void> ready)
{
    bool announced = false;
    int timer      = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    try {
        if (timer < 0) throw std::runtime_error("cockpit timerfd failed");
        network::EventLoop loop;
        network::Channel work(&loop, wake_.fd()), clock(&loop, timer);
        auto arm = [&] {
            auto deadline = std::max(now_ns() + 1, next_deadline(now_ns()));
            itimerspec value{};
            value.it_value.tv_sec  = deadline / second;
            value.it_value.tv_nsec = deadline % second;
            timerfd_settime(timer, TFD_TIMER_ABSTIME, &value, nullptr);
        };
        work.setReadCallback([&] {
            wake_.drain();
            if (stopping_) {
                loop.quit();
                return;
            }
            for (size_t source = 0; source < source_count; ++source) {
                auto fault = ingress_.fault(source);
                if (fault == faults_[source]) continue;
                faults_[source] = fault;
                context_.invalidate_source(source, fault >> 32, static_cast<uint32_t>(fault));
            }
            auto available   = signals_.read_available();
            max_queue_depth_ = std::max<uint64_t>(max_queue_depth_, available);
            if (!prefix_remaining_) prefix_remaining_ = available;
            SignalUpdate update;
            for (size_t count = 0; count < 32 && prefix_remaining_ && signals_.pop(update); ++count) {
                auto dequeued = now_ns();
                context_.apply(update);
                auto applied = now_ns();
                signal_latency_[0].add(update.enqueue_time_ns - update.receive_time_ns);
                signal_latency_[1].add(dequeued - update.enqueue_time_ns);
                signal_latency_[2].add(applied - dequeued);
                signal_latency_[3].add(applied - update.receive_time_ns);
                ++source_updates_[update.sample.source_id];
                source_latency_[update.sample.source_id].add(applied - update.receive_time_ns);
                --prefix_remaining_;
            }
            ingress_.consumed();
            // 只等本轮已经入队的前缀，不等持续输入的队列完全为空。
            if (prefix_remaining_) {
                wake_.notify();
                return;
            }
            context_.advance(now_ns());
            if (result_overflow_.exchange(false)) {
                for (size_t i = 0; i < bindings_.size(); ++i) unavailable(i, "result mailbox overflow");
                if (service_)
                    finish_service(service_->instance.state == ServiceState::Executing ? ServiceState::Unknown
                                                                                       : ServiceState::Failed,
                                   "result mailbox overflow");
            }
            Control request;
            for (size_t i = 0; i < 16 && controls_.pop(request); ++i) {
                try {
                    control(request);
                } catch (const std::exception& e) {
                    error(request.context, e.what());
                }
            }
            Json message;
            for (size_t i = 0; i < 32 && results_.pop(message); ++i) {
                try {
                    result(message);
                } catch (const std::exception& e) {
                    record({{"kind", "protocol_error"}, {"reason", e.what()}});
                }
            }
            tick(now_ns());
            pump();
            arm();
            if (signals_.read_available() || controls_.size() || results_.size()) wake_.notify();
        });
        clock.setReadCallback([&] {
            uint64_t expirations;
            if (read(timer, &expirations, sizeof(expirations)) == sizeof(expirations)) wake_.notify();
        });
        work.enableReading();
        clock.enableReading();
        arm();
        record({{"kind", "startup"}, {"phase", "PERCEPTION_READY"}, {"latency_ns", now_ns() - started_ns_}});
        ready.set_value();
        announced = true;
        wake_.notify();
        loop.loop();
        work.disableAll();
        work.remove();
        clock.disableAll();
        clock.remove();
        if (service_)
            finish_service(
                service_->instance.state == ServiceState::Executing ? ServiceState::Unknown : ServiceState::Cancelled,
                "shutdown");
        while (!candidates_.empty()) finish_candidate(candidates_.begin()->first, "shutdown");
        while (!calls_.empty()) finish_call(calls_.begin()->first, "shutdown");
        if (question_) finish_question("cancelled", "shutdown");
        record({{"kind", "summary"}, {"status", status()}});
    } catch (...) {
        if (!announced)
            ready.set_exception(std::current_exception());
        else
            record({{"kind", "fatal"}, {"reason", "core event loop failed"}});
    }
    if (timer >= 0) close(timer);
}
Time CockpitTask::next_deadline(Time now) const
{
    Time deadline = std::min(context_.next_deadline(), now + 250 * millisecond);
    if (!session_.token.empty()) deadline = std::min(deadline, session_.expires);
    if (service_) deadline = std::min(deadline, service_->instance.deadline_ns);
    for (const auto& entry : candidates_)
        if (!service_ || service_->instance.candidate_id != entry.first)
            deadline = std::min(deadline, entry.second.deadline_ns);
    for (const auto& entry : calls_)
        deadline = std::min({deadline, entry.second.pending.deadline_ns, entry.second.retry_ns});
    return deadline;
}
Json CockpitTask::status() const
{
    Json capabilities = Json::object();
    const char* names[]{"asr", "llm", "tts"};
    const char* states[]{"UNBOUND", "PROBING", "READY", "UNAVAILABLE"};
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const auto& binding    = bindings_[i];
        capabilities[names[i]] = {{"work_id", binding.work},
                                  {"generation", binding.generation},
                                  {"state", states[static_cast<int>(binding.state)]},
                                  {"reason", binding.reason}};
    }
    Json signals = Json::object();
    for (size_t i = 0; i < signal_count; ++i) {
        const auto& signal      = context_.signals()[i];
        signals[signal_name(i)] = {{"value", signal.value},
                                   {"status", static_cast<int>(signal.status)},
                                   {"version", signal.version},
                                   {"receive_time_ns", signal.update_time_ns}};
    }
    const auto& ingress = ingress_.stats();
    Json metrics{{"ingress", signal_latency_[0].report()}, {"queue", signal_latency_[1].report()},
                 {"context", signal_latency_[2].report()}, {"perception", signal_latency_[3].report()},
                 {"max_queue_depth", max_queue_depth_},    {"received", ingress.received.load()},
                 {"filtered", ingress.filtered.load()},    {"coalesced", ingress.coalesced.load()},
                 {"overflow", ingress.overflow.load()},    {"malformed", ingress.malformed.load()},
                 {"trace_dropped", trace_.dropped()},      {"output_dropped", output_dropped_.load()},
                 {"source_updates", source_updates_}};
    metrics["sources"] = Json::object();
    for (size_t source = 0; source < source_count; ++source)
        if (source_updates_[source]) metrics["sources"][std::to_string(source)] = source_latency_[source].report();
    metrics["raw_processing_mean_ns"] =
        ingress.received.load() ? Json(double(ingress.processing_ns.load()) / ingress.received.load()) : Json(nullptr);
    metrics["raw_processing_max_ns"] = ingress.processing_max_ns.load();
    Json service                     = nullptr;
    if (service_)
        service = {{"service_id", service_->instance.service_id},
                   {"candidate_id", service_->instance.candidate_id},
                   {"state", static_cast<int>(service_->instance.state)},
                   {"deadline_ns", service_->instance.deadline_ns}};
    return {{"runtime_id", runtime_id_},
            {"perception_ready", !stopping_},
            {"capabilities", capabilities},
            {"binding_version", binding_version_},
            {"interaction_ready", session_valid(now_ns())},
            {"paused", paused_},
            {"signals", signals},
            {"updates", context_.updates()},
            {"rejected", context_.rejected()},
            {"candidates", candidates_.size()},
            {"pending_calls", calls_.size()},
            {"service", service},
            {"metrics", metrics}};
}
}  // namespace cockpit
