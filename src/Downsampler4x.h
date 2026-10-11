#pragma once

#include <array>

namespace plasticz {

// Original four-section recursive 4:1 decimator from 0x1001c..0x104ba.
class Downsampler4x {
public:
    void reset() noexcept { history_.fill(0.0f); }
    // Input contains four consecutive high-rate samples per output frame.
    // Histories are retained between calls; output replaces the mono buffer.
    void process(const float* input, float* output, int outputFrames) noexcept;
    // Original 0xfeb7 cleanup occurs once per voice-render call, after all
    // samples, not at scratch-buffer boundaries or while 4x mode is disabled.
    void finishBlock() noexcept;
    const std::array<float, 10>& history() const noexcept { return history_; }

private:
    float step(float input) noexcept;
    // Original object +4..+28: input pair, then four section-output pairs.
    std::array<float, 10> history_{};
};

} // namespace plasticz
