#pragma once
// Small dependency-free DSP blocks used around the NAM model.
#include <algorithm>
#include <cmath>
#include <vector>

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    inline float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = (float)(B0 / A0);
        b1 = (float)(B1 / A0);
        b2 = (float)(B2 / A0);
        a1 = (float)(A1 / A0);
        a2 = (float)(A2 / A0);
    }
    void lowShelf(double fs, double f, double dB) {
        double A = std::pow(10.0, dB / 40.0), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w);
        double al = s / 2.0 * std::sqrt(2.0), t = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - t),
            (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t);
    }
    void highShelf(double fs, double f, double dB) {
        double A = std::pow(10.0, dB / 40.0), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w);
        double al = s / 2.0 * std::sqrt(2.0), t = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * c + t), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - t),
            (A + 1) - (A - 1) * c + t, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - t);
    }
    void peaking(double fs, double f, double dB, double Q) {
        double A = std::pow(10.0, dB / 40.0), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w);
        double al = s / (2 * Q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
};

// Three-band tone stack (bass shelf / mid bell / treble shelf).
struct ToneStack {
    Biquad lo, mid, hi;
    float cb = 1e9f, cm = 1e9f, ct = 1e9f;  // cached params
    void update(double fs, float bass, float m, float treble) {
        if (bass != cb) { lo.lowShelf(fs, 120.0, bass); cb = bass; }
        if (m != cm) { mid.peaking(fs, 750.0, m, 0.8); cm = m; }
        if (treble != ct) { hi.highShelf(fs, 3500.0, treble); ct = treble; }
    }
    inline float process(float x) { return hi.process(mid.process(lo.process(x))); }
    bool flat() const { return cb == 0 && cm == 0 && ct == 0; }
};

// Simple noise gate with envelope follower and smoothed gain.
struct NoiseGate {
    float env = 0, g = 1;
    float relEnv = 0, atk = 0, rel = 0;
    void init(double fs) {
        relEnv = (float)std::exp(-1.0 / (0.08 * fs));
        atk = (float)(1.0 - std::exp(-1.0 / (0.001 * fs)));
        rel = (float)(1.0 - std::exp(-1.0 / (0.060 * fs)));
    }
    void process(float* x, int n, float thresholdDb) {
        if (thresholdDb <= -89.0f) { g = 1; return; }
        const float thr = dbToLin(thresholdDb);
        for (int i = 0; i < n; ++i) {
            float a = std::fabs(x[i]);
            env = a > env ? a : env * relEnv;
            float target = env > thr ? 1.0f : 0.0f;
            g += (target - g) * (target > g ? atk : rel);
            x[i] *= g;
        }
    }
};

// Direct-form FIR (cab IR). Cost = L MACs per sample; keep L <= 512 on the H700.
struct FirConv {
    std::vector<float> hr;    // reversed impulse response
    std::vector<float> hist;  // doubled circular history (2*L)
    int L = 0, pos = 0;

    void setIR(const std::vector<float>& h) {
        L = (int)h.size();
        hr.assign(h.rbegin(), h.rend());
        hist.assign((size_t)L * 2, 0.0f);
        pos = 0;
    }
    void clear() { L = 0; hr.clear(); hist.clear(); pos = 0; }
    void process(float* x, int n) {
        if (L == 0) return;
        const float* k = hr.data();
        for (int i = 0; i < n; ++i) {
            hist[pos] = hist[pos + L] = x[i];
            pos = (pos + 1) % L;
            const float* w = &hist[pos];
            float acc = 0;
            for (int j = 0; j < L; ++j) acc += k[j] * w[j];
            x[i] = acc;
        }
    }
};

// Prepare a raw IR for FirConv: trim, fade tail, normalise energy.
inline std::vector<float> prepareIR(std::vector<float> ir, size_t maxLen = 512) {
    if (ir.empty()) return ir;
    if (ir.size() > maxLen) ir.resize(maxLen);
    const size_t fade = std::min<size_t>(ir.size() / 8, 64);
    for (size_t i = 0; i < fade; ++i) ir[ir.size() - 1 - i] *= (float)i / (float)fade;
    double e = 0;
    for (float v : ir) e += (double)v * v;
    if (e > 1e-12) {
        float s = (float)(1.0 / std::sqrt(e));
        for (float& v : ir) v *= s;
    }
    return ir;
}
