#include "vehicle_simulator.h"

namespace cockpit {
VehicleSimulator::VehicleSimulator(Ingress& ingress, std::function<bool(Json)> result, const Json& config)
    : ingress_(ingress),
      result_(std::move(result)),
      mode_(config.value("mode", "success")),
      delay_(config.value("delay_ms", 20) * millisecond),
      ac_on_(config.value("ac_on", false))
{
    if ((mode_ != "success" && mode_ != "failure" && mode_ != "timeout") || delay_ < 0 || delay_ > 60 * second)
        throw std::runtime_error("invalid vehicle simulator configuration");
}
VehicleSimulator::~VehicleSimulator()
{
    stop();
}
void VehicleSimulator::start()
{
    thread_ = std::thread([this] { run(); });
}
void VehicleSimulator::stop()
{
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}
bool VehicleSimulator::execute(ServiceInstance service)
{
    if (!commands_.push(service)) return false;
    cv_.notify_one();
    return true;
}
void VehicleSimulator::readback()
{
    SignalFrame frame;
    frame.signal_id = AirConditioner;
    frame.source_id = source_count - 1;  // 保留给车辆服务模拟器，回放器不产生 hvac.on。
    frame.sequence  = ++sequence_;
    frame.value     = ac_on_ ? 1 : 0;
    ingress_.inject(frame);
}
void VehicleSimulator::run()
{
    ServiceInstance current{};
    bool active       = false;
    Time completed_at = never;
    while (!stopping_) {
        readback();
        if (!active) {
            ServiceInstance service;
            if (commands_.pop(service) && service.service_id > last_service_) {
                if (now_ns() >= service.deadline_ns) {
                    last_service_ = service.service_id;
                    result_({{"object", "cockpit.vehicle.result"},
                             {"data",
                              {{"service_id", service.service_id},
                               {"phase", "completed"},
                               {"success", false},
                               {"reason", "command expired before execution"},
                               {"time_ns", now_ns()}}}});
                    continue;
                }
                current       = service;
                active        = true;
                last_service_ = service.service_id;
                completed_at  = now_ns() + delay_;
                result_({{"object", "cockpit.vehicle.result"},
                         {"data", {{"service_id", service.service_id}, {"phase", "accepted"}, {"time_ns", now_ns()}}}});
            }
        }
        if (active && now_ns() >= completed_at) {
            // 执行前再次检查截止时间，超时命令不能在迟到后改变车辆状态。
            const bool expired = now_ns() >= current.deadline_ns;
            if (!expired && mode_ == "success" && current.service_type == Service::AirConditioner) {
                ac_on_ = current.enabled;
                readback();
            }
            if (mode_ != "timeout")
                result_(
                    {{"object", "cockpit.vehicle.result"},
                     {"data",
                      {{"service_id", current.service_id},
                       {"phase", "completed"},
                       {"success", !expired && mode_ == "success"},
                       {"reason", expired ? "command expired" : mode_ == "success" ? "" : "simulated vehicle failure"},
                       {"time_ns", now_ns()}}}});
            active = false;
        }
        auto wait = active ? std::min<Time>(250 * millisecond, std::max<Time>(millisecond, completed_at - now_ns()))
                           : 250 * millisecond;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::nanoseconds(wait), [&] { return stopping_ || (!active && commands_.size()); });
    }
}
}  // namespace cockpit
