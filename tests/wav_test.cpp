#include "wav.h"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace StackFlows;
int main()
{
    auto root = std::filesystem::temp_directory_path() / ("edge-wav-test-" + std::to_string(getpid()));
    std::filesystem::create_directory(root);
    try {
        std::vector<int16_t> pcm(16000, 1024);
        auto path = root / "roundtrip.wav";
        {
            WavWriter writer(path);
            writer.append(pcm.data(), pcm.size());
            writer.finish();
        }
        auto decoded = read_voice_wav(path);
        if (decoded.size() != pcm.size() || decoded.front() != 1024.0f / 32768.0f)
            throw std::runtime_error("WAV roundtrip mismatch");
        path = root / "unfinished.wav";
        {
            WavWriter writer(path);
            writer.append(pcm.data(), pcm.size());
        }
        if (std::filesystem::exists(path) || std::filesystem::exists(path.string() + ".part"))
            throw std::runtime_error("unfinished WAV was not cleaned up");
        path = root / "full-device.wav";
        std::filesystem::create_symlink("/dev/full", path.string() + ".part");
        bool failed = false;
        try {
            WavWriter writer(path);
            writer.append(pcm.data(), pcm.size());
            writer.finish();
        } catch (const std::exception&) {
            failed = true;
        }
        if (!failed || std::filesystem::exists(path) || std::filesystem::is_symlink(path.string() + ".part"))
            throw std::runtime_error("write failure did not remove partial output");
        std::filesystem::remove_all(root);
        std::cout << "WAV roundtrip, unfinished cleanup and mid-write failure passed\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
