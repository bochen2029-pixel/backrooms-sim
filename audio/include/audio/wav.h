#pragma once
//
// audio/wav.h — minimal RIFF/WAVE PCM16 read/write (header-only, no deps).
//
// The build targets a single little-endian x64 toolchain, so POD fwrite/fread
// of the header fields is byte-correct (same assumption as replay_v1). Shared by
// the offline `--render-wav` path and the `wavcheck` gate tool.
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace br::audio {

// Write interleaved PCM16 (`channels`-interleaved) as a canonical 44-byte-header
// WAVE file. Returns false (with err set) on any I/O failure.
inline bool write_wav(const std::string& path, const std::vector<int16_t>& samples,
                      uint32_t sample_rate, uint16_t channels, std::string& err) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open WAV for write: " + path; return false; }
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint16_t bits = 16;
    const uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t byte_rate = sample_rate * block_align;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36u + data_bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16u); u16(1u /*PCM*/); u16(channels);
    u32(sample_rate); u32(byte_rate); u16(block_align); u16(bits);
    std::fwrite("data", 1, 4, f); u32(data_bytes);
    if (!samples.empty()) std::fwrite(samples.data(), sizeof(int16_t), samples.size(), f);
    const bool ok = (std::ferror(f) == 0);
    std::fclose(f);
    if (!ok) { err = "WAV write I/O error: " + path; return false; }
    return true;
}

// Read a PCM16 WAVE file into interleaved samples. Skips unknown chunks.
inline bool read_wav(const std::string& path, std::vector<int16_t>& samples,
                     uint32_t& sample_rate, uint16_t& channels, std::string& err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open WAV: " + path; return false; }
    char riff[4], wave[4];
    uint32_t riff_size = 0;
    const bool head = std::fread(riff, 1, 4, f) == 4 && std::fread(&riff_size, 4, 1, f) == 1 &&
                      std::fread(wave, 1, 4, f) == 4;
    if (!head || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        std::fclose(f); err = "not a RIFF/WAVE file: " + path; return false;
    }
    uint16_t fmt = 0, ch = 0, bits = 0, block = 0;
    uint32_t sr = 0, br = 0;
    bool have_fmt = false, have_data = false;
    char id[4];
    uint32_t sz = 0;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&sz, 4, 1, f) == 1) {
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::fread(&fmt, 2, 1, f); std::fread(&ch, 2, 1, f); std::fread(&sr, 4, 1, f);
            std::fread(&br, 4, 1, f); std::fread(&block, 2, 1, f); std::fread(&bits, 2, 1, f);
            have_fmt = true;
            if (sz > 16) std::fseek(f, static_cast<long>(sz - 16), SEEK_CUR);
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t n = sz / sizeof(int16_t);
            samples.resize(n);
            if (n && std::fread(samples.data(), sizeof(int16_t), n, f) != n) {
                std::fclose(f); err = "short data chunk: " + path; return false;
            }
            have_data = true;
        } else {
            std::fseek(f, static_cast<long>(sz), SEEK_CUR);
        }
        if (sz & 1u) std::fseek(f, 1, SEEK_CUR);  // chunks are word-aligned
    }
    std::fclose(f);
    if (!have_fmt || !have_data) { err = "missing fmt/data chunk: " + path; return false; }
    if (fmt != 1 || bits != 16) { err = "not PCM16: " + path; return false; }
    sample_rate = sr; channels = ch;
    return true;
}

// Clamp a float sample in [-1,1] to PCM16.
inline int16_t to_pcm16(float v) {
    float c = v;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    int x = static_cast<int>(c * 32767.0f);
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return static_cast<int16_t>(x);
}

}  // namespace br::audio
