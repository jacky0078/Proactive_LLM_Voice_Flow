#include "context.h"
#include "ingress.h"
#include "metrics.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace cockpit;
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
int main()
{
    try {
        std::vector<Candidate> candidates;
        ContextConfig config;
        config.signal_ttl = 10 * second;
        Context context(config, [&](const Candidate& value) { candidates.push_back(value); });
        uint32_t sequence = 0;
        Time time         = second;
        auto set          = [&](Signal signal, double value, size_t seat = 0, bool valid = true) {
            SignalFrame frame;
            frame.signal_id = signal_id(signal, seat);
            frame.sequence  = ++sequence;
            frame.value     = value;
            frame.quality   = valid;
            check(context.apply({frame, time, time}), "accept valid update");
        };
        set(Temperature, 30);
        set(AirConditioner, 0);
        set(Speed, 40);
        set(Occupied, 1);
        set(Fanning, .9);
        context.advance(time);
        check(candidates.empty(), "heat needs observation");
        time += 800 * millisecond;
        context.advance(time);
        check(candidates.size() == 1 && need_type(candidates.back().need_id) == Need::Heat,
              "heat deadline fires without input");
        auto first   = candidates.back();
        auto version = context.signals()[Temperature].version;
        set(Temperature, 30);
        check(context.signals()[Temperature].version == version, "freshness-only updates preserve semantic version");
        check(context.valid(first, time), "candidate still valid");
        set(AirConditioner, 1);
        check(!context.valid(first, time), "vehicle readback invalidates heat proposal");
        context.finish(first.need_id, time);
        set(AirConditioner, 0);
        set(Fanning, .9);
        context.advance(time);
        time += 800 * millisecond;
        context.advance(time);
        check(candidates.back().candidate_id > first.candidate_id, "candidate IDs never reused");
        context.invalidate_source(0, 1, sequence);
        check(!context.valid(candidates.back(), time), "loss invalidates evidence");
        SignalFrame old;
        old.signal_id = Temperature;
        old.sequence  = sequence;
        old.value     = 30;
        check(!context.apply({old, time, time}), "queued pre-loss sample cannot restore context");

        std::vector<Candidate> slow;
        Context comfort(config, [&](const Candidate& value) { slow.push_back(value); });
        auto comfort_set = [&](Signal signal, double value, size_t seat = 0) {
            SignalFrame frame;
            frame.signal_id = signal_id(signal, seat);
            frame.sequence  = ++sequence;
            frame.value     = value;
            comfort.apply({frame, time, time});
        };
        comfort_set(Temperature, 25);
        comfort_set(AirConditioner, 0);
        comfort_set(Speed, 20);
        comfort_set(Occupied, 1);
        comfort_set(Discomfort, .9);
        comfort.advance(time);
        check(slow.empty(), "single discomfort event does not trigger");
        time += millisecond;
        comfort_set(Discomfort, .9);
        comfort.advance(time);
        check(slow.size() == 1 && need_type(slow[0].need_id) == Need::Comfort, "two distinct events trigger slow path");
        comfort_set(Occupied, 1, 1);
        check(comfort.valid(slow[0], time, true), "unrelated seat does not invalidate snapshot");
        comfort_set(Temperature, 26);
        check(!comfort.valid(slow[0], time, true), "related version invalidates slow result");
        comfort.advance(time + config.signal_ttl);
        check(comfort.signals()[Temperature].status == Status::Stale, "TTL expires without new events");

        // 一批中的离座和回座仍应撤销旧候选；不同来源首次上线不影响本座位。
        std::vector<Candidate> rest;
        Context fatigue(config, [&](const Candidate& value) { rest.push_back(value); });
        auto fatigue_set = [&](Signal signal, double value, uint16_t source = 0, size_t seat = 0) {
            SignalFrame frame;
            frame.signal_id = signal_id(signal, seat);
            frame.source_id = source;
            frame.sequence  = ++sequence;
            frame.value     = value;
            check(fatigue.apply({frame, time, time}), "fatigue update");
        };
        fatigue_set(Speed, 40);
        fatigue_set(Occupied, 1);
        fatigue_set(Fatigue, .9);
        fatigue.advance(time);
        time += second;
        fatigue_set(Occupied, 1, 2, 1);
        fatigue.advance(time);
        fatigue_set(Occupied, 0);
        fatigue_set(Occupied, 1);
        fatigue.advance(time);
        time += second;
        fatigue.advance(time);
        check(rest.empty(), "brief condition loss resets observation within a batch");
        time += second;
        fatigue.advance(time);
        check(rest.size() == 1, "fatigue confirms after fresh continuous observation");
        fatigue_set(Occupied, 0);
        fatigue_set(Occupied, 1);
        check(!fatigue.valid(rest[0], time), "restored condition cannot revive old candidate");
        fatigue.invalidate_source(2, 1, sequence);
        check(fatigue.signals()[signal_id(Occupied)].status == Status::Valid,
              "unrelated source failure preserves driver signals");

        Timers timers;
        for (int i = 0; i < 10000; ++i) timers.set(7, i + 1);
        check(timers.size() == 1, "timer replacements do not accumulate stale entries");
        uint32_t timer = 0;
        check(!timers.pop(9999, timer) && timers.pop(10000, timer) && timer == 7, "timer deadline");
        Histogram histogram;
        for (int i = 1; i <= 1000; ++i) histogram.add(i * 1000);
        auto report = histogram.report();
        check(report["count"] == 1000 && report["max_ms"] == 1.0, "latency histogram");

        SignalQueue queue;
        WakeFd wake;
        auto prefix = std::string("/tmp/cockpit-core-") + std::to_string(getpid());
        Ingress ingress(queue, wake, {prefix + "-a", prefix + "-b"});
        ingress.start();
        int sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        for (int i = 0; i < 2; ++i) {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            auto path          = prefix + (i == 0 ? "-a" : "-b");
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            SignalFrame frame;
            frame.signal_id = Temperature + i;
            frame.source_id = i;
            frame.sequence  = 1;
            frame.value     = i;
            check(sendto(sender, &frame, sizeof(frame), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
                      sizeof(frame),
                  "send FD signal");
        }
        Time deadline = now_ns() + second;
        while (queue.read_available() < 2 && now_ns() < deadline) std::this_thread::yield();
        check(queue.read_available() == 2, "multiple FDs share one sensory queue");
        SignalUpdate update;
        while (queue.pop(update)) check(update.enqueue_time_ns >= update.receive_time_ns, "signal timestamp order");
        ingress.consumed();
        for (uint32_t i = 1; i <= 1400; ++i) {
            SignalFrame frame;
            frame.signal_id = signal_id(Discomfort);
            frame.source_id = 2;
            frame.sequence  = i;
            frame.value     = .9;
            while (!ingress.inject(frame)) std::this_thread::yield();
        }
        deadline = now_ns() + second;
        while (!ingress.fault(2) && now_ns() < deadline) std::this_thread::yield();
        check(ingress.fault(2) != 0, "must-retain overflow is reported outside full SPSC");
        ingress.stop();
        close(sender);
        std::cout << "cockpit core: context, FSM, freshness, IDs, timers, multi-FD ingress and overflow passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
