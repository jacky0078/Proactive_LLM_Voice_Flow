#include "wav.h"
#include <array>
#include <cmath>
#include <stdexcept>
using namespace StackFlows;
namespace {
uint32_t u32(const unsigned char* p)
{
    return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint16_t u16(const unsigned char* p)
{
    return p[0] | uint16_t(p[1]) << 8;
}
void write_u32(std::ostream& out, uint32_t n)
{
    char p[4] = {char(n), char(n >> 8), char(n >> 16), char(n >> 24)};
    out.write(p, 4);
}
void write_u16(std::ostream& out, uint16_t n)
{
    char p[2] = {char(n), char(n >> 8)};
    out.write(p, 2);
}
}  // namespace
std::vector<float> StackFlows::read_voice_wav(const std::string& path)
{
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("cannot open WAV: " + path);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open WAV: " + path);
    std::array<unsigned char, 12> header{};
    file.read(reinterpret_cast<char*>(header.data()), 12);
    if (!file || std::string(reinterpret_cast<char*>(header.data()), 4) != "RIFF" ||
        std::string(reinterpret_cast<char*>(header.data() + 8), 4) != "WAVE")
        throw std::runtime_error("invalid RIFF/WAVE header");
    bool format_ok = false;
    while (file) {
        unsigned char chunk[8];
        file.read(reinterpret_cast<char*>(chunk), 8);
        if (!file) break;
        auto size = u32(chunk + 4);
        std::string kind(reinterpret_cast<char*>(chunk), 4);
        if (size > 16 * 1024 * 1024) throw std::runtime_error("WAV chunk too large");
        if (kind == "fmt ") {
            if (size < 16) throw std::runtime_error("invalid WAV format chunk");
            std::vector<unsigned char> format(size);
            file.read(reinterpret_cast<char*>(format.data()), size);
            format_ok = file && u16(format.data()) == 1 && u16(format.data() + 2) == 1 &&
                        u32(format.data() + 4) == 16000 && u16(format.data() + 12) == 2 &&
                        u16(format.data() + 14) == 16;
            if (!format_ok) throw std::runtime_error("WAV must be 16000 Hz mono 16-bit PCM");
        } else if (kind == "data") {
            if (!format_ok || !size || size % 2 || size > 16000 * 2 * 120)
                throw std::runtime_error("invalid WAV data (maximum 120 seconds)");
            std::vector<unsigned char> pcm(size);
            file.read(reinterpret_cast<char*>(pcm.data()), size);
            if (!file) throw std::runtime_error("truncated WAV data");
            std::vector<float> samples(size / 2);
            double energy = 0;
            for (size_t i = 0; i < samples.size(); ++i) {
                samples[i] = static_cast<int16_t>(u16(pcm.data() + i * 2)) / 32768.0f;
                energy += samples[i] * samples[i];
            }
            if (energy / samples.size() < 1e-10) throw std::runtime_error("audio is silent");
            return samples;
        } else
            file.seekg(size, std::ios::cur);
        if (size & 1) file.seekg(1, std::ios::cur);
    }
    throw std::runtime_error("WAV has no PCM data");
}
WavWriter::WavWriter(const std::filesystem::path& path) : path_(path), temporary_(path.string() + ".part")
{
    file_.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        file_.open(temporary_, std::ios::binary | std::ios::trunc);
        header();
    } catch (...) {
        // 构造失败时析构函数不会运行，仍需清理可能创建的临时文件。
        try {
            if (file_.is_open()) file_.close();
        } catch (...) {
        }
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
        throw;
    }
}
WavWriter::~WavWriter()
{
    try {
        if (file_.is_open()) file_.close();
    } catch (...) {
    }
    if (!finished_) {
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
    }
}
void WavWriter::header()
{
    file_.seekp(0);
    file_.write("RIFF", 4);
    write_u32(file_, 36 + samples_ * 2);
    file_.write("WAVEfmt ", 8);
    write_u32(file_, 16);
    write_u16(file_, 1);
    write_u16(file_, 1);
    write_u32(file_, 16000);
    write_u32(file_, 32000);
    write_u16(file_, 2);
    write_u16(file_, 16);
    file_.write("data", 4);
    write_u32(file_, samples_ * 2);
}
void WavWriter::append(const int16_t* data, size_t count)
{
    if (!data || !count || count > 16000 * 120 || samples_ + count > 16000 * 600)
        throw std::runtime_error("invalid/oversized synthesized PCM");
    file_.write(reinterpret_cast<const char*>(data), count * sizeof(int16_t));
    samples_ += count;
}
void WavWriter::finish()
{
    if (!samples_) throw std::runtime_error("empty synthesized audio");
    header();
    file_.flush();
    file_.close();
    std::filesystem::rename(temporary_, path_);
    finished_ = true;
}
