#pragma once
#include <alsa/asoundlib.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "drift.h"
#include "dsp.h"
#include "nam_wrap.h"

struct Params {
    std::atomic<float> inGainDb{0.0f};
    std::atomic<float> gateDb{-90.0f};
    std::atomic<float> bass{0.0f};
    std::atomic<float> mid{0.0f};
    std::atomic<float> treble{0.0f};
    std::atomic<float> masterDb{-6.0f};
};

class Engine {
public:
    static constexpr int kMaxBlock = 2048;

    Params p;
    std::atomic<float> peakIn{0}, peakOut{0}, cpuLoad{0};
    std::atomic<int> xruns{0};
    std::atomic<bool> modelBypassed{false};  // set by the audio thread when the model is too heavy for real time
    std::string capDev = "default", playDev = "default";
    int rate = 48000;
    int period = 128;
    std::string captureNote;  // why capture is unavailable (if so)

    ~Engine() { stop(); }

    bool hasCapture() const { return cap_ != nullptr; }
    int capChannels() const { return capCh_; }

    // Opens the devices and starts the audio thread. Playback is mandatory,
    // capture is optional (loop-file mode still works without it).
    bool start(std::string& err) {
        xruns = 0;
        captureNote.clear();
        unsigned r = (unsigned)rate;
        snd_pcm_uframes_t per = (snd_pcm_uframes_t)std::min(std::max(period, 32), 1024);
        playCh_ = 2;
        play_ = openPcm(playDev.c_str(), SND_PCM_STREAM_PLAYBACK, r, per, playCh_, err);
        if (!play_) return false;
        period = (int)per;
        rate = (int)r;

        unsigned rc = r;
        snd_pcm_uframes_t perc = per;
        capCh_ = 2;
        std::string cerr;
        if (capDev.empty()) {
            captureNote = "no input device selected";
        } else {
            cap_ = openPcm(capDev.c_str(), SND_PCM_STREAM_CAPTURE, rc, perc, capCh_, cerr);
            if (!cap_) captureNote = cerr;
            else if ((int)rc != rate) {
                captureNote = "capture rate differs from playback rate";
                snd_pcm_close(cap_);
                cap_ = nullptr;
            }
        }

        gate_.init(rate);
        split_ = cap_ != nullptr && capDev != playDev;  // two different sound cards = two clocks
        if (split_) drift_.init(period);
        running_ = true;
        if (split_) capTh_ = std::thread(&Engine::captureRun, this);
        th_ = std::thread(&Engine::run, this);
        return true;
    }

    bool splitClocks() const { return split_; }

    void stop() {
        running_ = false;
        if (th_.joinable()) th_.join();
        if (capTh_.joinable()) capTh_.join();
        if (cap_) { snd_pcm_close(cap_); cap_ = nullptr; }
        if (play_) { snd_pcm_close(play_); play_ = nullptr; }
    }

    void setModel(std::shared_ptr<NamModel> m) {
        std::atomic_store(&model_, std::move(m));
        overBlocks_ = 0;
        modelBypassed = false;
    }
    void setIR(std::shared_ptr<std::vector<float>> ir) { std::atomic_store(&ir_, std::move(ir)); }
    void setLoop(std::shared_ptr<std::vector<float>> l) { std::atomic_store(&loop_, std::move(l)); }

private:
    snd_pcm_t* cap_ = nullptr;
    snd_pcm_t* play_ = nullptr;
    int capCh_ = 2, playCh_ = 2;
    std::thread th_, capTh_;
    bool split_ = false;
    DriftBuffer drift_;
    std::atomic<bool> running_{false};
    std::atomic<int> overBlocks_{0};

    std::shared_ptr<NamModel> model_;
    std::shared_ptr<std::vector<float>> ir_, loop_;

    // audio-thread state
    ToneStack tone_;
    NoiseGate gate_;
    FirConv fir_;
    const std::vector<float>* irSeen_ = nullptr;
    const std::vector<float>* loopSeen_ = nullptr;
    size_t loopPos_ = 0;

    static snd_pcm_t* openPcm(const char* dev, snd_pcm_stream_t st, unsigned& rate, snd_pcm_uframes_t& period,
                              int& ch, std::string& err) {
        snd_pcm_t* h = nullptr;
        int e = snd_pcm_open(&h, dev, st, 0);
        if (e < 0) { err = std::string(dev) + ": " + snd_strerror(e); return nullptr; }
        snd_pcm_hw_params_t* hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(h, hw);
        auto bad = [&](const char* what, int code) {
            err = std::string(dev) + ": " + what + ": " + snd_strerror(code);
            snd_pcm_close(h);
            return (snd_pcm_t*)nullptr;
        };
        if ((e = snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) return bad("access", e);
        // FLOAT_LE works through the "plug" layer: use e.g. plughw:1,0 as device name.
        if ((e = snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_FLOAT_LE)) < 0)
            return bad("float format (try a plughw: device)", e);
        unsigned c = (unsigned)ch;
        if ((e = snd_pcm_hw_params_set_channels_near(h, hw, &c)) < 0) return bad("channels", e);
        ch = (int)c;
        if ((e = snd_pcm_hw_params_set_rate_near(h, hw, &rate, nullptr)) < 0) return bad("rate", e);
        if ((e = snd_pcm_hw_params_set_period_size_near(h, hw, &period, nullptr)) < 0) return bad("period", e);
        snd_pcm_uframes_t buf = period * 3;
        if ((e = snd_pcm_hw_params_set_buffer_size_near(h, hw, &buf)) < 0) return bad("buffer", e);
        if ((e = snd_pcm_hw_params(h, hw)) < 0) return bad("hw_params", e);
        return h;
    }

    static void setRealtime() {
        sched_param sp{};
        sp.sched_priority = 60;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            // not permitted (not root): continue with normal priority
        }
    }

    void processBlock(float* x, int n) {
        float inDb = p.inGainDb.load(std::memory_order_relaxed);
        const float inG = dbToLin(inDb);
        float pk = 0;
        for (int i = 0; i < n; ++i) { x[i] *= inG; pk = std::max(pk, std::fabs(x[i])); }
        peakIn.store(std::max(peakIn.load() * 0.9f, pk), std::memory_order_relaxed);

        gate_.process(x, n, p.gateDb.load(std::memory_order_relaxed));

        auto model = std::atomic_load(&model_);
        if (model && !modelBypassed.load(std::memory_order_relaxed)) model->process(x, x, n);

        tone_.update(rate, p.bass.load(std::memory_order_relaxed), p.mid.load(std::memory_order_relaxed),
                     p.treble.load(std::memory_order_relaxed));
        if (!tone_.flat())
            for (int i = 0; i < n; ++i) x[i] = tone_.process(x[i]);

        auto ir = std::atomic_load(&ir_);
        if (ir.get() != irSeen_) {
            irSeen_ = ir.get();
            if (ir && !ir->empty()) fir_.setIR(*ir); else fir_.clear();
        }
        fir_.process(x, n);

        const float mG = dbToLin(p.masterDb.load(std::memory_order_relaxed));
        pk = 0;
        for (int i = 0; i < n; ++i) {
            float v = x[i] * mG;
            v = std::max(-1.0f, std::min(1.0f, v));
            x[i] = v;
            pk = std::max(pk, std::fabs(v));
        }
        peakOut.store(std::max(peakOut.load() * 0.9f, pk), std::memory_order_relaxed);
    }

    // Capture thread, only used when input and output are different sound cards.
    void captureRun() {
        setRealtime();
        const int n = period;
        std::vector<float> in((size_t)n * capCh_ + 8), mono((size_t)n);
        snd_pcm_prepare(cap_);
        snd_pcm_start(cap_);
        while (running_) {
            snd_pcm_sframes_t got = snd_pcm_readi(cap_, in.data(), n);
            if (got < 0) {
                if (snd_pcm_recover(cap_, (int)got, 1) < 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));  // unplugged?
                snd_pcm_start(cap_);
                xruns++;
                continue;
            }
            for (int i = 0; i < got; ++i) mono[(size_t)i] = in[(size_t)i * capCh_];
            drift_.push(mono.data(), (int)got);
        }
    }

    void run() {
        setRealtime();
        const int n = period;
        std::vector<float> inI((size_t)n * capCh_ + 8), outI((size_t)n * playCh_), mono((size_t)n);
        const double periodSec = (double)n / (double)rate;
        bool capLive = split_;  // in split mode the capture thread owns the capture stream
        bool wasLoop = false;
        const int overLimit = std::max(20, (int)(0.4 * rate / n));

        // prime playback so the first write does not underrun
        std::fill(outI.begin(), outI.end(), 0.0f);
        snd_pcm_writei(play_, outI.data(), n);

        while (running_) {
            auto loop = std::atomic_load(&loop_);
            if (loop.get() != loopSeen_) { loopSeen_ = loop.get(); loopPos_ = 0; }

            if (loop && !loop->empty()) {
                if (capLive && !split_) { snd_pcm_drop(cap_); capLive = false; }
                wasLoop = true;
                const std::vector<float>& L = *loop;
                for (int i = 0; i < n; ++i) {
                    mono[(size_t)i] = L[loopPos_];
                    if (++loopPos_ >= L.size()) loopPos_ = 0;
                }
            } else if (split_) {
                if (wasLoop) { drift_.flush(); wasLoop = false; }
                drift_.pull(mono.data(), n);  // resampled to follow the playback clock
            } else if (cap_) {
                wasLoop = false;
                if (!capLive) { snd_pcm_prepare(cap_); snd_pcm_start(cap_); capLive = true; }
                snd_pcm_sframes_t got = snd_pcm_readi(cap_, inI.data(), n);
                if (got < 0) {
                    if (snd_pcm_recover(cap_, (int)got, 1) < 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    snd_pcm_start(cap_);
                    xruns++;
                    got = 0;
                }
                for (int i = 0; i < n; ++i) mono[(size_t)i] = (i < got) ? inI[(size_t)i * capCh_] : 0.0f;
            } else {
                std::fill(mono.begin(), mono.end(), 0.0f);
            }

            auto t0 = std::chrono::steady_clock::now();
            processBlock(mono.data(), n);
            auto t1 = std::chrono::steady_clock::now();
            float load = (float)(std::chrono::duration<double>(t1 - t0).count() / periodSec);
            cpuLoad.store(cpuLoad.load() * 0.9f + load * 0.1f, std::memory_order_relaxed);
            // Protection: if the model needs more than the available time for ~0.4 s, bypass it
            // so the console stays responsive (the UI then offers Lite or tells the user).
            if (load > 1.0f) {
                if (++overBlocks_ > overLimit) modelBypassed.store(true);
            } else {
                overBlocks_ = 0;
            }

            for (int i = 0; i < n; ++i)
                for (int c = 0; c < playCh_; ++c) outI[(size_t)i * playCh_ + c] = mono[(size_t)i];
            snd_pcm_sframes_t w = snd_pcm_writei(play_, outI.data(), n);
            if (w < 0) {
                if (snd_pcm_recover(play_, (int)w, 1) < 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                xruns++;
            }
        }
    }
};

// ---------------------------------------------------------------- audio card list
struct AudioCard {
    std::string label;  // e.g. "NUX MG-30"
    std::string dev;    // ALSA name, e.g. "plughw:1,0"
    bool capture = false, playback = false;
};

inline std::vector<AudioCard> listAudioCards() {
    std::vector<AudioCard> out;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[32];
        snprintf(hw, sizeof hw, "hw:%d", card);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
        std::string label = "card " + std::to_string(card);
        snd_ctl_card_info_t* ci;
        snd_ctl_card_info_alloca(&ci);
        if (snd_ctl_card_info(ctl, ci) == 0) label = snd_ctl_card_info_get_name(ci);
        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
            snd_pcm_info_t* pi;
            snd_pcm_info_alloca(&pi);
            snd_pcm_info_set_device(pi, (unsigned)dev);
            snd_pcm_info_set_subdevice(pi, 0);
            snd_pcm_info_set_stream(pi, SND_PCM_STREAM_CAPTURE);
            const bool cap = snd_ctl_pcm_info(ctl, pi) == 0;
            snd_pcm_info_set_stream(pi, SND_PCM_STREAM_PLAYBACK);
            const bool play = snd_ctl_pcm_info(ctl, pi) == 0;
            if (cap || play) {
                out.push_back({label, "plughw:" + std::to_string(card) + "," + std::to_string(dev), cap, play});
                break;
            }
        }
        snd_ctl_close(ctl);
    }
    return out;
}
