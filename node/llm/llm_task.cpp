#include "voice_task.h"
#include "stream_text.h"
#ifdef EDGE_HAS_RKLLM
#include "rkllm_backend.h"
#endif
using namespace StackFlows;
namespace {
class LlmTask : public VoiceTask {
public:
    LlmTask(std::shared_ptr<llm_channel_obj> channel, const Json& config) : VoiceTask(std::move(channel), config)
    {
        auto backend = config.at("backend").get<std::string>();
        if (backend == "rkllm") {
#ifdef EDGE_HAS_RKLLM
            rkllm_ = std::make_unique<RkLlmBackend>(config);
#else
            throw std::runtime_error("this build has no RKLLM runtime; rebuild with EDGE_ENABLE_RKLLM=ON on AArch64");
#endif
        } else if (backend != "mock")
            throw std::runtime_error("backend must be mock or rkllm");
    }
    ~LlmTask() override
    {
        stop();
    }

private:
#ifdef EDGE_HAS_RKLLM
    std::unique_ptr<RkLlmBackend> rkllm_;
#endif
    void abort() override
    {
#ifdef EDGE_HAS_RKLLM
        if (rkllm_) rkllm_->abort();
#endif
    }
    void process(const VoiceMessage& message) override
    {
        auto object = message.body.at("object").get<std::string>();
        if (object != "asr.utf-8" && object != "llm.utf-8") throw std::runtime_error("LLM expects final UTF-8 text");
        auto text = message.body.at("data").get<std::string>();
        if (text.empty() || text.size() > 16384) throw std::runtime_error("empty/oversized LLM input");
        int index   = 0;
        auto output = [&](const std::string& part) {
            emit(message.context, "llm.utf-8.stream", {{"index", index++}, {"delta", part}, {"finish", false}});
        };
#ifdef EDGE_HAS_RKLLM
        if (rkllm_)
            rkllm_->run(text, output);
        else
#endif
        {
            int delay = config_.value("mock_delay_ms", 0);
            if (delay < 0 || delay > 30000) throw std::runtime_error("invalid mock delay");
            if (wait_or_stop(delay)) throw std::runtime_error("mock inference stopped");
            auto reply = config_.value("mock_reply", "收到，我是车载语音助手。你刚才说：" + text + "。");
            StreamText stream(output);
            stream.append(reply);
            stream.finish();
        }
        emit(message.context, "llm.utf-8.stream", {{"index", index}, {"delta", ""}, {"finish", true}}, true);
    }
};
}  // namespace
std::shared_ptr<VoiceTask> StackFlows::make_voice_task(std::shared_ptr<llm_channel_obj> channel, const Json& config)
{
    return std::make_shared<LlmTask>(std::move(channel), config);
}
