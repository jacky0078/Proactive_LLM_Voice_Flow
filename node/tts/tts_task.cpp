#include "voice_task.h"
#include "wav.h"
#include "SynthesizerTrn.h"
#include <openssl/sha.h>
#include <omp.h>
#include <array>
#include <iomanip>
#include <sstream>
using namespace StackFlows;
namespace {
// SummerTTS 原模型解析器不检查所有偏移；在进入原解析器前核对已知模型摘要。
std::vector<float> load_model(const Json& config)
{
    auto path = config.at("model_path").get<std::string>();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open TTS model: " + path);
    auto size = file.tellg();
    if (size < 16 || size % 4 || size > 1024 * 1024 * 1024) throw std::runtime_error("invalid TTS model size");
    std::vector<float> model(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(model.data()), size);
    if (!file) throw std::runtime_error("cannot read complete TTS model");
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(model.data()), size, digest);
    std::ostringstream hash;
    for (auto byte : digest) hash << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
    auto expected = config.value("model_sha256", "87b774813258a3636b3f1d9adddd3652ede503b4e8f4eb3c39065cba2a4951c4");
    if (hash.str() != expected)
        throw std::runtime_error("TTS model checksum mismatch (use the matching SummerTTS model)");
    return model;
}
class TtsTask : public VoiceTask {
public:
    TtsTask(std::shared_ptr<llm_channel_obj> channel, const Json& config)
        : VoiceTask(std::move(channel), config),
          model_(load_model(config)),
          synthesizer_(std::make_unique<SynthesizerTrn>(model_.data(), model_.size() * sizeof(float)))
    {
        output_dir_ = std::filesystem::absolute(config.at("output_dir").get<std::string>());
        std::filesystem::create_directories(output_dir_);
        timeout_ = config.value("stream_timeout_seconds", 120.0);
        threads_ = config.value("num_threads", 2);
        if (timeout_ <= 0 || threads_ < 1 || threads_ > 16) throw std::runtime_error("invalid TTS timeout/threads");
    }
    ~TtsTask() override
    {
        stop();
    }

private:
    std::vector<float> model_;
    std::unique_ptr<SynthesizerTrn> synthesizer_;
    std::filesystem::path output_dir_, current_path_;
    std::unique_ptr<WavWriter> writer_;
    RequestContext current_;
    size_t file_index_  = 0;
    int expected_index_ = 0, threads_ = 2;
    double timeout_ = 120;
    std::chrono::steady_clock::time_point last_fragment_;
    bool accepts_fragments() const override
    {
        return true;
    }
    void reset_request() override
    {
        writer_.reset();
        current_        = {};
        expected_index_ = 0;
    }
    void on_idle() override
    {
        if (writer_ &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - last_fragment_).count() > timeout_) {
            auto context = current_;
            reset_request();
            fail(context, "TTS stream timed out waiting for finish");
        }
    }
    void process(const VoiceMessage& message) override
    {
        auto object = message.body.at("object").get<std::string>();
        if (object != "llm.utf-8.stream" && object != "tts.utf-8")
            throw std::runtime_error("TTS expects llm.utf-8.stream or tts.utf-8");
        Json data = message.body.at("data");
        if (object == "tts.utf-8") {
            if (!data.is_string() || data.get<std::string>().empty()) throw std::runtime_error("TTS text is empty");
            data = {{"index", 0}, {"delta", data}, {"finish", true}};
        }
        int index   = data.at("index").get<int>();
        auto text   = data.at("delta").get<std::string>();
        bool finish = data.at("finish").get<bool>();
        if (text.size() > 4096) throw std::runtime_error("TTS text fragment too long");
        if (index != expected_index_)
            throw std::runtime_error("TTS missing, duplicate or out-of-order stream fragment");
        if (!writer_) {
            if (index != 0) throw std::runtime_error("TTS stream must start at index 0");
            current_    = message.context;
            auto unique = std::chrono::system_clock::now().time_since_epoch().count();
            current_path_ =
                output_dir_ / ("voice-" + std::to_string(unique) + "-" + std::to_string(file_index_++) + ".wav");
            writer_ = std::make_unique<WavWriter>(current_path_);
        }
        if (current_.request_id != message.context.request_id) throw std::runtime_error("TTS request context mismatch");
        ++expected_index_;
        if (!text.empty() && text.find_first_not_of(" \r\n\t") != std::string::npos) {
            // 原 SummerTTS infer；任务线程按片段顺序合成并写入同一个 WAV。
            omp_set_num_threads(threads_);
            int32_t count = 0;
            std::unique_ptr<int16_t, decltype(&std::free)> audio(synthesizer_->infer(text, 0, 1.0f, count), &std::free);
            if (!audio || count <= 0) throw std::runtime_error("SummerTTS synthesis returned no samples");
            writer_->append(audio.get(), count);
        }
        last_fragment_ = std::chrono::steady_clock::now();
        if (finish) {
            writer_->finish();
            Json result{{"path", current_path_.string()},
                        {"sample_rate", 16000},
                        {"channels", 1},
                        {"bits_per_sample", 16},
                        {"samples", writer_->samples()}};
            reset_request();
            emit(message.context, "tts.audio.file", result, true);
        }
    }
};
}  // namespace
std::shared_ptr<VoiceTask> StackFlows::make_voice_task(std::shared_ptr<llm_channel_obj> channel, const Json& config)
{
    return std::make_shared<TtsTask>(std::move(channel), config);
}
