#pragma once
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace spt {
// Fixed sample count: source speed is controlled by the WASAPI stream clock.
// Independent pitch is compensated by 2^(semitones/12) / appliedTempo.
class PitchProcessor {
    signalsmith::stretch::SignalsmithStretch<float> stretch_{0};
    int channels_ = 0, bits_ = 0, validBits_ = 0, rate_ = 0;
    bool floating_ = false, processing_ = false;
    double factor_ = 1;
    int warmup_ = 0, settling_ = 0;
    float wet_ = 0;
    static constexpr int block_ = 1024;
    std::vector<std::vector<float>> input_, output_;
public:
    void configure(int channels, int rate, int bits, bool floating, int validBits = 0) {
        channels_ = channels; rate_ = rate; bits_ = bits; floating_ = floating;
        validBits_ = validBits ? validBits : bits;
        stretch_.presetDefault(channels, float(rate), true);
        input_.assign(channels, std::vector<float>(block_));
        output_.assign(channels, std::vector<float>(block_));
        reset();
    }
    int latency() const { return stretch_.inputLatency() + stretch_.outputLatency(); }
    void reset() { stretch_.reset(); stretch_.setTransposeFactor(1.f); processing_ = false; factor_ = 1; warmup_ = settling_ = 0; wet_ = 0; }
    bool settled() const { return !processing_ || (wet_ == 1 && warmup_ == 0 && settling_ == 0); }
    bool wet() const { return processing_; }
    double acceptedFactor() const { return processing_ ? factor_ : 1; }
    float decode(const uint8_t* p) const {
        if (floating_) { float v; std::memcpy(&v, p, 4); return std::isfinite(v) ? v : 0; }
        if (bits_ == 16) { int16_t v; std::memcpy(&v, p, 2); return v / 32768.f; }
        if (bits_ == 24) {
            int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
            if (v & 0x800000) v |= int32_t(0xff000000);
            return v / 8388608.f;
        }
        int32_t v; std::memcpy(&v, p, 4); return float(v / 2147483648.0);
    }
    void encode(uint8_t* p, float v) const {
        if (!std::isfinite(v)) v = 0;
        v = std::clamp(v, -1.f, 1.f);
        if (floating_) { std::memcpy(p, &v, 4); return; }
        int64_t scale = int64_t(1) << (validBits_ - 1);
        int64_t quantized = std::clamp(std::llround(v*double(scale)), -scale, scale-1);
        uint32_t packed = uint32_t(quantized) << (bits_ - validBits_);
        for (int i = 0; i < bits_/8; ++i) p[i] = uint8_t(packed >> (8*i));
    }
    void process(uint8_t* bytes, unsigned frames, double factor) {
        if (!channels_ || !bytes || !std::isfinite(factor)) return;
        bool bypass = std::abs(factor - 1.0) < 0.00001;
        if (bypass && !processing_) return; // Unity is bit-exact.
        if (!processing_) { stretch_.reset(); warmup_ = latency(); processing_ = true; wet_ = 0; }
        if (std::abs(factor - factor_) > 0.000001) {
            stretch_.setTransposeFactor(float(factor)); factor_ = factor;
            settling_ = latency();
        }
        int bytesPerSample = bits_/8;
        for (unsigned pos = 0; pos < frames;) {
            int count = int(std::min<unsigned>(block_, frames - pos));
            auto* start = bytes + size_t(pos)*channels_*bytesPerSample;
            for (int n = 0; n < count; ++n)
                for (int c = 0; c < channels_; ++c)
                    input_[c][n] = decode(start + size_t(n*channels_ + c)*bytesPerSample);
            stretch_.process(input_, count, output_, count);
            for (int n = 0; n < count; ++n) {
                if (warmup_ > 0) --warmup_;
                if (settling_ > 0) --settling_;
                float target = (!bypass && warmup_ == 0) ? 1.f : 0.f;
                float step = 1.f / (rate_*0.025f);
                wet_ += std::clamp(target - wet_, -step, step);
                if (wet_ == 0) continue; // Preserve dry bytes, including PCM padding.
                for (int c = 0; c < channels_; ++c) {
                    float v = input_[c][n] + wet_*(output_[c][n] - input_[c][n]);
                    encode(start + size_t(n*channels_ + c)*bytesPerSample, v);
                }
            }
            pos += count;
        }
        if (bypass && wet_ == 0) { processing_ = false; factor_ = 1; }
    }
};
}
