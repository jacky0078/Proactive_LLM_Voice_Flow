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
        backend_ = config.at("backend").get<std::string>();
        if (backend_ == "rkllm") {
#ifdef EDGE_HAS_RKLLM
            rkllm_ = std::make_unique<RkLlmBackend>(config);
#else
            throw std::runtime_error("this build has no RKLLM runtime; rebuild with EDGE_ENABLE_RKLLM=ON on AArch64");
#endif
        } else if (backend_ != "mock")
            throw std::runtime_error("backend must be mock or rkllm");
    }
    ~LlmTask() override
    {
        stop();
    }

private:
    std::string backend_;
#ifdef EDGE_HAS_RKLLM
    std::unique_ptr<RkLlmBackend> rkllm_;
#endif
    bool supports_cancel() const override
    {
        return true;
    }
    void cancel_active() override
    {
#ifdef EDGE_HAS_RKLLM
        if (rkllm_) rkllm_->cancel();
#endif
    }
    void abort() override
    {
#ifdef EDGE_HAS_RKLLM
        if (rkllm_) rkllm_->abort();
#endif
    }
    void process(const VoiceMessage& message) override
    {
        auto object   = message.body.at("object").get<std::string>();
        bool decision = object == "llm.decision.request";
        if (!decision && object != "asr.utf-8" && object != "llm.utf-8")
            throw std::runtime_error("LLM expects final UTF-8 text or a decision request");
        std::string text;
        if (decision) {
            if (!message.body.at("data").is_object()) throw std::runtime_error("decision input must be an object");
            text =
                "根据座舱数据判断是否建议开启空调。只输出一个JSON对象，不输出解释。"
                "可选格式：{\"service\":\"air_conditioner\",\"params\":{\"enabled\":true}}"
                "或{\"service\":\"no_action\"}。所有行为都需要用户确认。数据：" +
                message.body.at("data").dump();
        } else
            text = message.body.at("data").get<std::string>();
        if (text.empty() || text.size() > 16384) throw std::runtime_error("empty/oversized LLM input");
        int index = 0;
        std::string result;
        auto output = [&](const std::string& part) {
            if (cancelled() || stopping()) throw std::runtime_error("request cancelled");
            if (decision) {
                if (result.size() + part.size() > 8192) throw std::runtime_error("decision output too large");
                result += part;
            } else
                emit(message.context, "llm.utf-8.stream", {{"index", index++}, {"delta", part}, {"finish", false}});
        };
#ifdef EDGE_HAS_RKLLM
        if (rkllm_)
            rkllm_->run(text, output);
        else
#endif
        {
            int delay = config_.value("mock_delay_ms", 0);
            auto mode = config_.value("mock_decision_mode", "valid");
            if (decision && mode == "timeout") delay = 30000;
            if (delay < 0 || delay > 30000) throw std::runtime_error("invalid mock delay");
            if (wait_or_stop(delay)) throw std::runtime_error("request cancelled");
            if (decision) {
                if (mode == "invalid")
                    output("invalid decision");
                else if (mode == "no_action")
                    output(R"({"service":"no_action"})");
                else
                    output(R"({"service":"air_conditioner","params":{"enabled":true}})");
            } else {
                auto reply = config_.value("mock_reply", "收到，我是车载语音助手。你刚才说：" + text + "。");
                StreamText stream(output);
                stream.append(reply);
                stream.finish();
            }
        }
        if (cancelled()) throw std::runtime_error("request cancelled");
        if (decision)
            emit(message.context, "llm.decision.result", Json::parse(result), true);
        else
            emit(message.context, "llm.utf-8.stream", {{"index", index}, {"delta", ""}, {"finish", true}}, true);
    }
};
}  // namespace
std::shared_ptr<VoiceTask> StackFlows::make_voice_task(std::shared_ptr<llm_channel_obj> channel, const Json& config)
{
    return std::make_shared<LlmTask>(std::move(channel), config);
}
