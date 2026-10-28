#pragma once
#include "ingress.h"
#include "json.hpp"
#include <condition_variable>
#include <functional>

namespace cockpit {
class VehicleSimulator {
public:
    using Json = nlohmann::json;
    VehicleSimulator(Ingress& ingress, std::function<bool(Json)> result, const Json& config);
    ~VehicleSimulator();
    void start();
    void stop();
    bool execute(ServiceInstance service);

private:
    Ingress& ingress_;
    std::function<bool(Json)> result_;
    Mailbox<ServiceInstance> commands_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::string mode_;
    Time delay_;
    bool ac_on_            = false;
    uint32_t sequence_     = 0;
    uint64_t last_service_ = 0;
    void run();
    void readback();
};
}  // namespace cockpit
