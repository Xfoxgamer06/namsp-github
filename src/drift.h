#pragma once
// Bridges two audio clocks (e.g. USB interface input -> built-in speaker output).
// Producer = capture thread (push), consumer = playback thread (pull).
// The consumer resamples by a tiny, adaptive ratio so the buffer fill stays at the
// target: no clicks from the two sound cards drifting apart.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

class DriftBuffer {
public:
    void init(int period) {
        size_t cap = 8192;
        while (cap < (size_t)period * 16) cap <<= 1;
        buf_.assign(cap, 0.0f);
        mask_ = cap - 1;
        w_ = 0;
        r_ = 0;
        rd_ = 0;
        frac_ = 0.0;
        ratio_ = 1.0;
        trim_ = 0.0;
        last_ = 0.0f;
        primed_ = false;
        target_ = (double)period * 3.0;
        underruns = 0;
        overruns = 0;
    }

    // capture thread
    void push(const float* x, int n) {
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const uint64_t r = r_.load(std::memory_order_acquire);
        size_t freeSpace = buf_.size() - (size_t)(w - r);
        size_t cnt = (size_t)n;
        if (cnt > freeSpace) {
            cnt = freeSpace;
            overruns++;
        }
        for (size_t i = 0; i < cnt; ++i) buf_[(size_t)((w + i) & mask_)] = x[i];
        w_.store(w + cnt, std::memory_order_release);
    }

    // playback thread: fills out[0..n) ; returns false while still priming (silence)
    bool pull(float* out, int n) {
        const uint64_t w = w_.load(std::memory_order_acquire);
        double fill = (double)(w - rd_) - frac_;
        if (!primed_) {
            if (fill < target_) {
                std::fill(out, out + n, 0.0f);
                return false;
            }
            primed_ = true;
        }
        // proportional control: too full -> consume slightly faster, too empty -> slower
        // (PI: the integral part learns the steady clock offset, so the fill returns to target)
        const double err = (fill - target_) / target_;
        trim_ = std::max(-0.01, std::min(0.01, trim_ + 5e-6 * err * ((double)n / 128.0)));
        ratio_ = 1.0 + std::max(-0.012, std::min(0.012, trim_ + 0.004 * err));
        for (int i = 0; i < n; ++i) {
            if (w - rd_ < 2) {  // starved: hold the last value
                out[i] = last_;
                underruns++;
                continue;
            }
            const float a = buf_[(size_t)(rd_ & mask_)], b = buf_[(size_t)((rd_ + 1) & mask_)];
            out[i] = last_ = a + (b - a) * (float)frac_;
            frac_ += ratio_;
            while (frac_ >= 1.0) {
                frac_ -= 1.0;
                ++rd_;
            }
        }
        r_.store(rd_, std::memory_order_release);
        return true;
    }

    // playback thread: forget everything buffered (e.g. when leaving loop mode)
    void flush() {
        rd_ = w_.load(std::memory_order_acquire);
        frac_ = 0.0;
        primed_ = false;
        r_.store(rd_, std::memory_order_release);
    }

    double fillSamples() const { return (double)(w_.load() - rd_) - frac_; }

    std::atomic<int> underruns{0}, overruns{0};

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    std::atomic<uint64_t> w_{0}, r_{0};
    uint64_t rd_ = 0;  // consumer-owned
    double frac_ = 0.0, ratio_ = 1.0, trim_ = 0.0, target_ = 384.0;
    float last_ = 0.0f;
    bool primed_ = false;
};
