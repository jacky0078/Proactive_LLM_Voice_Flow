// Adapted from LLM_Voice_Flow llm/rknn-llm/examples/
// DeepSeek-R1-Distill-Qwen-1.5B_Demo/deploy/src/llm_demo.cpp.
// 保留 Init/rkllm_run、采样默认值和 DeepSeek 模板，替换全局状态与固定端口通信。
#include "rkllm_backend.h"
#include "stream_text.h"
#include <cstddef>
#include <cstdint>
#include "rkllm.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <iostream>
using namespace StackFlows;
struct RkLlmBackend::Impl {
    LLMHandle handle = nullptr;
    std::string model_path, system_prompt;
    std::mutex handle_mutex;
    bool running   = false;
    bool stopped   = false;
    bool cancelled = false;
    struct Run {
        struct Event {
            std::string text;
            LLMCallState state;
        };
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Event> events;
        bool done     = false;
        bool finished = false;
        bool failed   = false;
        std::exception_ptr exception;
    };
    static void callback(RKLLMResult* result, void* userdata, LLMCallState state) noexcept
    {
        auto* run = static_cast<Run*>(userdata);
        if (!run) return;
        try {
            // SDK 的 text 指针仅在回调期间有效；复制后立即入队。
            Run::Event event{state == RKLLM_RUN_NORMAL && result && result->text ? result->text : "", state};
            {
                std::lock_guard<std::mutex> lock(run->mutex);
                if (run->exception) return;
                if (run->events.size() >= 128 || event.text.size() > 8192) {
                    run->exception = std::make_exception_ptr(std::runtime_error("RKLLM callback queue overflow"));
                } else {
                    run->events.push_back(std::move(event));
                }
            }
            run->cv.notify_one();
        } catch (...) {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->exception = std::current_exception();
            run->cv.notify_one();
        }
    }
};
RkLlmBackend::RkLlmBackend(const nlohmann::json& config) : impl_(std::make_unique<Impl>())
{
    impl_->model_path = config.at("model_path").get<std::string>();
    if (!std::filesystem::is_regular_file(impl_->model_path))
        throw std::runtime_error("RKLLM model file not found: " + impl_->model_path);
    impl_->system_prompt                 = config.value("system_prompt", "");
    RKLLMParam param                     = rkllm_createDefaultParam();
    param.model_path                     = impl_->model_path.c_str();
    param.top_k                          = config.value("top_k", 1);
    param.top_p                          = config.value("top_p", 0.95f);
    param.temperature                    = config.value("temperature", 0.8f);
    param.repeat_penalty                 = config.value("repeat_penalty", 1.1f);
    param.frequency_penalty              = config.value("frequency_penalty", 0.0f);
    param.presence_penalty               = config.value("presence_penalty", 0.0f);
    param.max_new_tokens                 = config.value("max_new_tokens", 100);
    param.max_context_len                = config.value("max_context_len", 256);
    param.skip_special_token             = true;
    param.is_async                       = false;
    param.extend_param.base_domain_id    = 0;
    param.extend_param.embed_flash       = 1;
    param.extend_param.enabled_cpus_num  = config.value("enabled_cpus_num", 2);
    param.extend_param.enabled_cpus_mask = config.value("enabled_cpus_mask", 5u);
    if (param.top_k < 1 || param.top_p <= 0 || param.top_p > 1 || param.temperature < 0 || param.max_new_tokens < 1 ||
        param.max_context_len < param.max_new_tokens)
        throw std::runtime_error("invalid RKLLM generation parameters");
    int rc = rkllm_init(&impl_->handle, &param, Impl::callback);
    if (rc || !impl_->handle) {
        if (impl_->handle) rkllm_destroy(impl_->handle);
        impl_->handle = nullptr;
        throw std::runtime_error("rkllm_init failed: " + std::to_string(rc));
    }
    rc = rkllm_set_chat_template(impl_->handle, impl_->system_prompt.c_str(), "<｜User｜>", "<｜Assistant｜>");
    if (rc) {
        rkllm_destroy(impl_->handle);
        impl_->handle = nullptr;
        throw std::runtime_error("rkllm_set_chat_template failed: " + std::to_string(rc));
    }
}
RkLlmBackend::~RkLlmBackend()
{
    // 任务先 abort + join 业务线程，再进入这里销毁模型。
    if (impl_->handle && rkllm_destroy(impl_->handle)) std::cerr << "rkllm_destroy failed\n";
}
void RkLlmBackend::abort()
{
    // abort 可能恰好先于 rkllm_run 进入 SDK；重复检查直到业务线程确认退出。
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(impl_->handle_mutex);
            impl_->stopped = true;
            if (!impl_->running) return;
            if (rkllm_abort(impl_->handle)) std::cerr << "rkllm_abort failed\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
void RkLlmBackend::cancel()
{
    std::lock_guard<std::mutex> lock(impl_->handle_mutex);
    if (!impl_->running) return;
    impl_->cancelled = true;
    if (rkllm_abort(impl_->handle)) std::cerr << "rkllm request cancellation failed\n";
}
void RkLlmBackend::run(const std::string& prompt, std::function<void(const std::string&)> output)
{
    Impl::Run run;
    {
        std::lock_guard<std::mutex> lock(impl_->handle_mutex);
        if (impl_->stopped) throw std::runtime_error("RKLLM task stopping");
        if (impl_->running) throw std::runtime_error("RKLLM task busy");
        impl_->running   = true;
        impl_->cancelled = false;
    }
    std::thread results([&] {
        try {
            StreamText text(std::move(output));
            for (;;) {
                Impl::Run::Event event;
                {
                    std::unique_lock<std::mutex> lock(run.mutex);
                    run.cv.wait(lock, [&] { return run.done || run.exception || !run.events.empty(); });
                    if (run.exception) break;
                    if (run.events.empty()) break;
                    event = std::move(run.events.front());
                    run.events.pop_front();
                }
                if (event.state == RKLLM_RUN_ERROR)
                    run.failed = true;
                else if (event.state == RKLLM_RUN_FINISH)
                    run.finished = true;
                else if (event.state == RKLLM_RUN_NORMAL && !run.finished)
                    text.append(event.text);
            }
            if (!run.failed && run.finished) text.finish();
        } catch (...) {
            std::lock_guard<std::mutex> lock(run.mutex);
            run.exception = std::current_exception();
        }
    });
    RKLLMInput input{};
    input.input_type   = RKLLM_INPUT_PROMPT;
    input.prompt_input = prompt.c_str();
    RKLLMInferParam param{};
    param.mode         = RKLLM_INFER_GENERATE;
    param.keep_history = 0;
    int rc             = rkllm_run(impl_->handle, &input, &param, &run);
    {
        std::lock_guard<std::mutex> lock(run.mutex);
        run.done = true;
    }
    run.cv.notify_one();
    results.join();
    bool stopped;
    {
        std::lock_guard<std::mutex> lock(impl_->handle_mutex);
        impl_->running = false;
        stopped        = impl_->stopped || impl_->cancelled;
    }
    if (run.exception) std::rethrow_exception(run.exception);
    if (rc || run.failed || !run.finished || stopped)
        throw std::runtime_error("rkllm_run failed/aborted or missing finish: " + std::to_string(rc));
}
