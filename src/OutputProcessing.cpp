#include "OutputProcessing.h"

#include <cmath>
#include <cstring>

// The original SSE arithmetic rounds after every operation. Prevent ARM FMA
// contraction even when this translation unit is built outside our CMake file.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace plasticz {
namespace {
float fromBits(std::uint32_t word) noexcept {
    float result;
    std::memcpy(&result, &word, sizeof(result));
    return result;
}
}

void OutputProcessing::prepare(float sampleRate) noexcept {
    // sub_2d3b4. Preserve the distinct attack/release division order.
    enabled_ = sampleRate > 0.0f && std::isfinite(sampleRate);
    if (enabled_) {
        const float attackSamples = sampleRate * 0.01f;
        attack_ = std::exp(-10.0f / attackSamples);
        const float releaseSamples = 0.01f * sampleRate;
        float exponent = 1.0f / releaseSamples;
        exponent = exponent * -10.0f;
        release_ = std::exp(exponent);
        float delay = 0.01f * sampleRate;
        delay = delay * 0.1f;
        lookahead_ = static_cast<std::uint32_t>(delay);
    }
    reset();
}

void OutputProcessing::reset() noexcept {
    filter_ = {};
    dc_ = {};
    delayLeft_.fill(0.0f);
    delayRight_.fill(0.0f);
    peakCount_ = 0;
    cursor_ = 0;
    heldPeak_ = 0.9f;
    smoothPeak_ = 0.9f;
}

void OutputProcessing::processInput(float* mono, int count) noexcept {
    // sub_24c70, mode 3, mono branch 0x24f7f. The filter is never exposed as
    // a synth parameter. sub_12c9e updates the FX object twice and leaves this
    // object's constructor rate at 44100, even when the host rate changes.
    // These are the constructor's stored f32 coefficients (+d0/+f4), derived
    // by its original instructions with the pinned oracle libSystem boundary.
    static constexpr std::uint32_t coefficients[2][5] = {
        {0x3f824d5e, 0xbffc9cdd, 0x3f74d39c, 0xbffca7ae, 0x3f7958b7},
        {0x406cb253, 0xc042b065, 0x3f8b16ba, 0x3f05d01f, 0x3e61942f},
    };
    const float bias = fromBits(0x219392ef);
    float smoothing = 100.0f * fromBits(0x3e999999);
    smoothing = smoothing / 44100.0f;
    for (int i = 0; i < count; ++i) {
        float value = mono[i] + bias;
        for (std::size_t stage = 0; stage < filter_.size(); ++stage) {
            auto& history = filter_[stage];
            const auto* c = coefficients[stage];
            float output = value * fromBits(c[0]);
            output = output + history.x1 * fromBits(c[1]);
            output = output + fromBits(c[2]) * history.x2;
            output = output - fromBits(c[3]) * history.y1;
            output = output - fromBits(c[4]) * history.y2;
            history.x2 = history.x1;
            history.x1 = value;
            history.y2 = history.y1;
            history.y1 = output;
            value = output;
        }
        for (auto& state : dc_) {
            float next = value - state;
            next = next * smoothing;
            next = next + state;
            state = next;
            value = value - next;
            value = value + bias;
        }
        mono[i] = value;
    }
}

void OutputProcessing::processOutput(float* left, float* right, int count) noexcept {
    if (!enabled_) return;
    // sub_2d6aa: stereo-linked peak hold, envelope and 1024-slot delay.
    constexpr float threshold = 0.9f;
    for (int i = 0; i < count; ++i) {
        const float inputLeft = left[i], inputRight = right[i];
        float level = std::fmax(std::fabs(inputLeft), std::fabs(inputRight));
        // SSE MAXSS chooses the second operand for unordered comparisons.
        level = level > threshold ? level : threshold;
        ++peakCount_;
        if (peakCount_ >= lookahead_ || level > heldPeak_) {
            peakCount_ = 0;
            heldPeak_ = level;
        } else {
            level = heldPeak_;
        }
        float next = smoothPeak_ - level;
        next = next * (level > smoothPeak_ ? attack_ : release_);
        next = level + next;
        smoothPeak_ = next;
        const float gain = threshold / smoothPeak_;
        const auto delayed = (cursor_ - lookahead_) & 1023u;
        const float delayedLeft = delayLeft_[delayed];
        const float delayedRight = delayRight_[delayed];
        delayLeft_[cursor_] = inputLeft;
        delayRight_[cursor_] = inputRight;
        cursor_ = (cursor_ + 1) & 1023u;
        left[i] = delayedLeft * gain;
        right[i] = gain * delayedRight;
    }
}

} // namespace plasticz
