#pragma once
#include "json.hpp"
#include <functional>
#include <memory>
#include <string>
namespace StackFlows {
// 仅适配原项目已有 RKLLM 实现；没有运行时插件或模拟降级。
class RkLlmBackend {
public:
    explicit RkLlmBackend(const nlohmann::json& config);
    ~RkLlmBackend();
    void run(const std::string& prompt, std::function<void(const std::string&)> output);
    void abort();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace StackFlows
