#include "voice_task.h"
#include "wav.h"
#include "sherpa-onnx/c-api/cxx-api.h"
#include <filesystem>
using namespace StackFlows;
namespace {
class AsrTask : public VoiceTask {
public:
    AsrTask(std::shared_ptr<llm_channel_obj> channel, const Json& config) : VoiceTask(std::move(channel), config)
    {
        sherpa_onnx::cxx::OnlineRecognizerConfig params;
        auto root                              = std::filesystem::path(config.at("model_dir").get<std::string>());
        params.model_config.transducer.encoder = (root / "encoder-epoch-99-avg-1.int8.onnx").string();
        params.model_config.transducer.decoder = (root / "decoder-epoch-99-avg-1.onnx").string();
        params.model_config.transducer.joiner  = (root / "joiner-epoch-99-avg-1.int8.onnx").string();
        params.model_config.tokens             = (root / "tokens.txt").string();
        for (const auto& path : {params.model_config.transducer.encoder, params.model_config.transducer.decoder,
                                 params.model_config.transducer.joiner, params.model_config.tokens})
            if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("ASR model file missing: " + path);
        params.model_config.num_threads = config.value("num_threads", 2);
        if (params.model_config.num_threads < 1 || params.model_config.num_threads > 16)
            throw std::runtime_error("invalid ASR num_threads");
        params.model_config.provider = "cpu";
        recognizer_ =
            std::make_unique<sherpa_onnx::cxx::OnlineRecognizer>(sherpa_onnx::cxx::OnlineRecognizer::Create(params));
        if (!recognizer_->Get()) throw std::runtime_error("Sherpa-ONNX model initialization failed");
    }
    ~AsrTask() override
    {
        stop();
    }

private:
    std::unique_ptr<sherpa_onnx::cxx::OnlineRecognizer> recognizer_;
    void process(const VoiceMessage& message) override
    {
        if (message.body.at("object") != "asr.audio.file") throw std::runtime_error("ASR expects asr.audio.file");
        auto samples = read_voice_wav(message.body.at("data").at("path").get<std::string>());
        auto stream  = recognizer_->CreateStream();
        if (!stream.Get()) throw std::runtime_error("ASR stream creation failed");
        stream.AcceptWaveform(16000, samples.data(), samples.size());
        // Zipformer 的尾帧需要少量静音填充；每轮创建新的识别流。
        std::vector<float> tail(8000, 0.0f);
        stream.AcceptWaveform(16000, tail.data(), tail.size());
        stream.InputFinished();
        while (!stopping() && recognizer_->IsReady(&stream)) recognizer_->Decode(&stream);
        if (stopping()) throw std::runtime_error("ASR stopped");
        auto text = recognizer_->GetResult(&stream).text;
        if (text.empty()) throw std::runtime_error("no speech recognized");
        emit(message.context, "asr.utf-8", text, true);
    }
};
}  // namespace
std::shared_ptr<VoiceTask> StackFlows::make_voice_task(std::shared_ptr<llm_channel_obj> channel, const Json& config)
{
    return std::make_shared<AsrTask>(std::move(channel), config);
}
