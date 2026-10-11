#pragma once

#include <array>
#include <cstdint>

namespace plasticz {

// Original fixed mono tone/DC filter, followed (after the synth's effects) by
// its stereo linked limiter. Compile without contraction or fast-math.
class OutputProcessing {
public:
    // Configures the output limiter and clears both stages' histories.
    void prepare(float sampleRate) noexcept;
    void reset() noexcept;
    void processInput(float* mono, int count) noexcept;
    void processOutput(float* left, float* right, int count) noexcept;
    int latencySamples() const noexcept { return enabled_ ? static_cast<int>(lookahead_ & 1023u) : 0; }

private:
    struct BiquadHistory { float x1{}, x2{}, y1{}, y2{}; };
    std::array<BiquadHistory, 2> filter_{};
    std::array<float, 4> dc_{};
    std::array<float, 1024> delayLeft_{}, delayRight_{};
    std::uint32_t lookahead_{}, peakCount_{}, cursor_{};
    float heldPeak_{0.9f}, smoothPeak_{0.9f}, attack_{}, release_{};
    bool enabled_{};
};

} // namespace plasticz
