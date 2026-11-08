#pragma once
// Minimal WAV reader: PCM 16/24/32, float32, any channel count (downmixed to mono).
// Assumes a little-endian host (true for ARM Linux / muOS).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct WavData {
    std::vector<float> mono;
    int rate = 0;
};

inline bool loadWav(const std::string& path, WavData& out, size_t maxFrames = 48000u * 300u) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    auto fail = [&]() { fclose(f); return false; };

    char id[4];
    uint32_t sz = 0;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) return fail();
    if (fread(&sz, 4, 1, f) != 1) return fail();
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) return fail();

    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    bool haveFmt = false;

    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            uint8_t b[40] = {0};
            uint32_t n = sz < 40 ? sz : 40;
            if (fread(b, 1, n, f) != n) return fail();
            if (sz > n) fseek(f, (long)(sz - n), SEEK_CUR);
            if (sz & 1) fseek(f, 1, SEEK_CUR);
            fmt = (uint16_t)(b[0] | (b[1] << 8));
            ch = (uint16_t)(b[2] | (b[3] << 8));
            rate = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
            bits = (uint16_t)(b[14] | (b[15] << 8));
            if (fmt == 0xFFFE && n >= 26) fmt = (uint16_t)(b[24] | (b[25] << 8));  // WAVE_FORMAT_EXTENSIBLE
            haveFmt = true;
        } else if (!memcmp(id, "data", 4)) {
            if (!haveFmt || ch == 0 || bits == 0 || (bits % 8) != 0) return fail();
            const int bps = bits / 8;
            if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) return fail();
            size_t frames = sz / ((size_t)ch * bps);
            if (frames > maxFrames) frames = maxFrames;
            std::vector<uint8_t> raw(frames * ch * bps);
            if (fread(raw.data(), 1, raw.size(), f) != raw.size()) {
                // truncated file: use what we got
                raw.clear();
                return fail();
            }
            out.mono.assign(frames, 0.0f);
            out.rate = (int)rate;
            for (size_t i = 0; i < frames; ++i) {
                float acc = 0;
                for (int c = 0; c < ch; ++c) {
                    const uint8_t* p = &raw[(i * ch + c) * bps];
                    float v;
                    if (fmt == 3) {
                        memcpy(&v, p, 4);
                    } else if (bits == 16) {
                        int16_t s;
                        memcpy(&s, p, 2);
                        v = s / 32768.0f;
                    } else if (bits == 24) {
                        int32_t s = (p[0] << 8) | (p[1] << 16) | ((int32_t)p[2] << 24);
                        v = (s >> 8) / 8388608.0f;
                    } else {
                        int32_t s;
                        memcpy(&s, p, 4);
                        v = s / 2147483648.0f;
                    }
                    acc += v;
                }
                out.mono[i] = acc / ch;
            }
            fclose(f);
            return true;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    return fail();
}

inline std::vector<float> resampleLinear(const std::vector<float>& in, double from, double to) {
    if (in.empty() || from <= 0 || to <= 0 || from == to) return in;
    const double ratio = from / to;
    const size_t n = (size_t)((double)in.size() / ratio);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        double pos = i * ratio;
        size_t i0 = (size_t)pos;
        double fr = pos - (double)i0;
        float a = in[i0];
        float b = (i0 + 1 < in.size()) ? in[i0 + 1] : a;
        out[i] = (float)(a + (b - a) * fr);
    }
    return out;
}
