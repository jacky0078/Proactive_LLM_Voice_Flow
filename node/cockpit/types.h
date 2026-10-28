#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <time.h>

namespace cockpit {
using Time                    = int64_t;
constexpr Time millisecond    = 1000000;
constexpr Time second         = 1000 * millisecond;
constexpr Time never          = std::numeric_limits<Time>::max();
constexpr size_t seat_count   = 4;
constexpr size_t signal_count = 4 + seat_count * 4;
constexpr size_t source_count = 16;
constexpr size_t need_count   = seat_count * 3;

inline Time now_ns()
{
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * second + time.tv_nsec;
}

enum Signal : uint16_t { Temperature, AirConditioner, Speed, Busy, Occupied, Fanning, Fatigue, Discomfort };
inline uint16_t signal_id(Signal signal, size_t seat = 0)
{
    return signal < Occupied ? static_cast<uint16_t>(signal) : static_cast<uint16_t>(4 + seat * 4 + signal - Occupied);
}
enum class Status : uint8_t { Invalid, Valid, Stale };
enum class Need : uint8_t { Heat, Rest, Comfort };
enum class Capability : uint8_t { Asr, Llm, Tts };
enum class Service : uint8_t { AirConditioner, Rest };
enum class ServiceState : uint8_t { Asking, Waiting, Executing, Succeeded, Failed, Unknown, Expired, Cancelled };

// 本机 FD 协议使用小端序、24 字节定长帧；接收和入队时间由 Ingress 填写。
struct SignalFrame {
    uint16_t signal_id         = 0;
    uint16_t source_id         = 0;
    uint32_t source_generation = 1;
    uint32_t sequence          = 0;
    uint8_t quality            = 1;
    uint8_t reserved[3]{};
    double value = 0;
};
static_assert(sizeof(SignalFrame) == 24, "signal wire layout changed");
struct SignalUpdate {
    SignalFrame sample;
    Time receive_time_ns = 0;
    Time enqueue_time_ns = 0;
};
struct SignalState {
    double value        = 0;
    Time update_time_ns = 0;
    uint32_t version    = 0;
    Status status       = Status::Invalid;
};
struct Candidate {
    uint64_t candidate_id = 0;
    uint16_t need_id      = 0;
    Time deadline_ns      = 0;
    Time observed_ns      = 0;
    Time confirmed_ns     = 0;
    // 仅慢路径使用：温度、空调、乘员、车速；没有完整 Context 副本。
    std::array<double, 4> values{};
    std::array<uint32_t, 4> versions{};
};
struct ServiceInstance {
    uint64_t service_id         = 0;
    uint64_t candidate_id       = 0;
    Service service_type        = Service::AirConditioner;
    ServiceState state          = ServiceState::Asking;
    Time deadline_ns            = 0;
    uint64_t session_generation = 0;
    // 首版空调服务只修改开关，休息服务只显示提示，不接受任意车控参数。
    bool enabled = true;
};
struct PendingCall {
    uint64_t call_id               = 0;
    uint64_t candidate_id          = 0;
    uint64_t service_id            = 0;
    Capability capability          = Capability::Asr;
    uint32_t capability_generation = 0;
    Time deadline_ns               = 0;
    Time started_ns                = 0;
};
inline Need need_type(uint16_t id)
{
    return static_cast<Need>(id % 3);
}
inline size_t need_seat(uint16_t id)
{
    return id / 3;
}
const char* signal_name(size_t id);
int find_signal(const std::string& name);
const char* need_name(Need need);
}  // namespace cockpit
