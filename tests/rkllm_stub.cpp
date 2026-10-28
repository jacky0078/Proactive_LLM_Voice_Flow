// 只用于测试的 SDK 替身。生产 edge_llm 从不链接此文件。
#include <cstddef>
#include <cstdint>
#include "rkllm.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
struct StubModel {
    LLMResultCallback callback;
    std::atomic<bool> aborted{false}, running{false};
    bool bad_template = false;
};
RKLLMParam stub_last_param{};
std::string stub_system_prompt;
std::atomic<int> stub_loaded{0}, stub_destroyed{0}, stub_runs{0}, stub_aborted{0};
extern "C" RKLLMParam rkllm_createDefaultParam()
{
    return {};
}
extern "C" int rkllm_init(LLMHandle* handle, RKLLMParam* param, LLMResultCallback callback)
{
    if (!handle || !param || !callback) return -1;
    if (std::string(param->model_path).find("init-fail") != std::string::npos) return -11;
    if (param->is_async || !param->skip_special_token) return -12;
    stub_last_param     = *param;
    auto* model         = new StubModel;
    model->callback     = callback;
    model->bad_template = std::string(param->model_path).find("template-fail") != std::string::npos;
    *handle             = model;
    ++stub_loaded;
    return 0;
}
extern "C" int rkllm_set_chat_template(LLMHandle handle, const char* system, const char* prefix, const char* postfix)
{
    auto* model = static_cast<StubModel*>(handle);
    if (model->bad_template) return -13;
    stub_system_prompt = system ? system : "";
    if (!system || std::string(prefix) != "<｜User｜>" || std::string(postfix) != "<｜Assistant｜>") return -14;
    return 0;
}
extern "C" int rkllm_run(LLMHandle handle, RKLLMInput* input, RKLLMInferParam* param, void* userdata)
{
    auto* model = static_cast<StubModel*>(handle);
    if (!userdata || param->keep_history || param->mode != RKLLM_INFER_GENERATE ||
        input->input_type != RKLLM_INPUT_PROMPT)
        return -15;
    model->aborted = false;
    ++stub_runs;
    model->running = true;
    std::string prompt(input->prompt_input);
    auto emit = [&](const std::string& text, LLMCallState state) {
        std::string temporary = text;
        RKLLMResult result{};
        result.text = temporary.c_str();
        model->callback(&result, userdata, state);
        temporary.assign(temporary.size(), 'x');  // 验证回调是否复制了 SDK 文本。
    };
    if (prompt == "abort") {
        while (!model->aborted) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        model->running = false;
        return -16;
    }
    if (prompt == "error") {
        emit("", RKLLM_RUN_ERROR);
        model->running = false;
        return 0;
    }
    if (prompt == "return-fail") {
        model->running = false;
        return -17;
    }
    if (prompt == "oversized-callback") {
        emit(std::string(8193, 'a'), RKLLM_RUN_NORMAL);
        emit("", RKLLM_RUN_FINISH);
        model->running = false;
        return 0;
    }
    emit("", RKLLM_RUN_WAITING);
    emit("<thi", RKLLM_RUN_NORMAL);
    emit("nk>不能播出的思考</th", RKLLM_RUN_NORMAL);
    emit("ink>你好，", RKLLM_RUN_NORMAL);
    emit("我是车载", RKLLM_RUN_NORMAL);
    emit("语音助手。", RKLLM_RUN_NORMAL);
    if (prompt != "missing-finish") emit("", RKLLM_RUN_FINISH);
    model->running = false;
    return 0;
}
extern "C" int rkllm_abort(LLMHandle handle)
{
    static_cast<StubModel*>(handle)->aborted = true;
    ++stub_aborted;
    return 0;
}
extern "C" int rkllm_is_running(LLMHandle handle)
{
    return static_cast<StubModel*>(handle)->running;
}
extern "C" int rkllm_destroy(LLMHandle handle)
{
    auto* model = static_cast<StubModel*>(handle);
    if (model->running) std::terminate();
    delete model;
    ++stub_destroyed;
    return 0;
}
