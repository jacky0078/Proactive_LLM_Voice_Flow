#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
namespace StackFlows {
std::vector<float> read_voice_wav(const std::string& path);
// 临时文件在 finish 后原子改名；失败或退出时删除未完成文件。
class WavWriter {
public:
    explicit WavWriter(const std::filesystem::path& path);
    ~WavWriter();
    void append(const int16_t* samples, size_t count);
    void finish();
    size_t samples() const
    {
        return samples_;
    }

private:
    std::filesystem::path path_, temporary_;
    std::ofstream file_;
    size_t samples_ = 0;
    bool finished_  = false;
    void header();
};
}  // namespace StackFlows
