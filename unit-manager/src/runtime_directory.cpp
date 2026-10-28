#include "runtime_directory.h"
#include "all.h"
#include "zmq_bus.h"
#include <chrono>
#include <mutex>
#include <unistd.h>

namespace {
std::mutex directory_mutex;
nlohmann::json directory;
int owner = -1;
const std::string manager_instance =
    std::to_string(getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}  // namespace
void runtime_action(int session, const nlohmann::json& request)
{
    using Json = nlohmann::json;
    if (request.at("work_id") != "sys") throw std::runtime_error("runtime directory belongs to sys");
    auto object      = request.at("object").get<std::string>();
    const auto& data = request.at("data");
    Json response;
    {
        std::lock_guard<std::mutex> lock(directory_mutex);
        if (object == "sys.runtime.publish") {
            auto runtime      = data.at("runtime_id").get<std::string>();
            const auto& tasks = data.at("tasks");
            if (runtime.empty() || runtime.size() > 128 || !tasks.is_object() || !tasks.contains("cockpit"))
                throw std::runtime_error("invalid runtime publication");
            if (!directory.is_null() && (owner != session || directory.at("runtime_id") != runtime))
                throw std::runtime_error("another runtime owns the directory");
            for (auto name : {"cockpit", "asr", "llm", "tts"}) {
                if (!tasks.contains(name) || tasks[name].is_null()) continue;
                auto work = tasks[name].get<std::string>();
                if (work.rfind(std::string(name) + ".", 0) != 0 || work.size() > 64)
                    throw std::runtime_error("task name does not match capability");
            }
            directory = {{"runtime_id", runtime}, {"tasks", tasks}, {"manager_instance", manager_instance}};
            owner     = session;
            response  = directory;
        } else if (object == "sys.runtime.lookup") {
            if (directory.is_null()) throw std::runtime_error("runtime not published");
            response = directory;
        } else if (object == "sys.runtime.remove") {
            if (!directory.is_null() && (owner != session || directory.at("runtime_id") != data.at("runtime_id")))
                throw std::runtime_error("runtime ownership mismatch");
            directory = nullptr;
            owner     = -1;
            response  = {{"removed", true}, {"manager_instance", manager_instance}};
        } else
            throw std::runtime_error("unsupported runtime directory operation");
    }
    Json message{{"request_id", request.at("request_id")},
                 {"work_id", "sys"},
                 {"created", time(nullptr)},
                 {"object", object},
                 {"data", response},
                 {"error", {{"code", 0}, {"message", ""}}}};
    zmq_com_send(session, message.dump());
}
