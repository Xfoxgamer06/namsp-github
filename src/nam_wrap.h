#pragma once
// ---------------------------------------------------------------------------
// The ONLY file that touches NeuralAmpModelerCore. API usage here mirrors what
// the official TONE3000 plugin does in plugin/src/NamEngine.cpp (v0.0.12):
//   - nam::get_dsp(path)                       -> std::unique_ptr<nam::DSP>
//   - dsp->ResetAndPrewarm(sampleRate, maxBlock)
//   - dsp->process(NAM_SAMPLE** in, NAM_SAMPLE** out, nFrames)   (NAM_SAMPLE = double)
//   - dynamic_cast<nam::SlimmableModel*>(dsp)->SetSlimmableSize(0.0 lite .. 1.0 full)
// Use the NeuralAmpModelerCore revision pinned by that plugin release.
// ---------------------------------------------------------------------------
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

class NamModel {
public:
    // slimSize: 0.0 = lite tier, 1.0 = full. Ignored by models that are not
    // slimmable containers. Heavy (prewarms the model): call from the UI thread only.
    bool load(const std::string& path, double sampleRate, int maxBlock, double slimSize, std::string& err) {
        try {
            dsp_ = nam::get_dsp(std::filesystem::path(path));
            if (!dsp_) {
                err = "model could not be created";
                return false;
            }
            dsp_->ResetAndPrewarm(sampleRate, maxBlock);
            slimmable_ = dynamic_cast<nam::SlimmableModel*>(dsp_.get()) != nullptr;
            if (auto* s = dynamic_cast<nam::SlimmableModel*>(dsp_.get())) s->SetSlimmableSize(slimSize);
            expectedRate_ = dsp_->GetExpectedSampleRate();
        } catch (const std::exception& e) {
            err = e.what();
            return false;
        } catch (...) {
            err = "unknown error while loading model";
            return false;
        }

        // Level-match to about -18 dBFS using the loudness stored in the file.
        outGain_ = 1.0f;
        if (dsp_->HasLoudness()) outGain_ = (float)std::pow(10.0, (-18.0 - dsp_->GetLoudness()) / 20.0);

        maxBlock_ = maxBlock;
        in_.assign((size_t)maxBlock, 0);
        out_.assign((size_t)maxBlock, 0);
        return true;
    }

    // Mono in -> mono out (in-place allowed). n must be <= maxBlock passed to load().
    void process(const float* in, float* out, int n) {
        for (int i = 0; i < n; ++i) in_[(size_t)i] = (NAM_SAMPLE)in[i];
        NAM_SAMPLE* ip[1] = {in_.data()};
        NAM_SAMPLE* op[1] = {out_.data()};
        dsp_->process(ip, op, n);
        for (int i = 0; i < n; ++i) out[i] = (float)out_[(size_t)i] * outGain_;
    }

    double expectedRate() const { return expectedRate_; }
    bool slimmable() const { return slimmable_; }
    int maxBlock() const { return maxBlock_; }

private:
    std::unique_ptr<nam::DSP> dsp_;
    std::vector<NAM_SAMPLE> in_, out_;
    double expectedRate_ = -1;
    float outGain_ = 1.0f;
    int maxBlock_ = 0;
    bool slimmable_ = false;
};
