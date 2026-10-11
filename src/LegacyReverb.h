#pragma once

#include <array>
#include <vector>

namespace plasticz {

// Recovered from the original 0x25a8c object. Parameter numbers belong to this
// effect object, not the plug-in's 38-parameter interface.
class LegacyReverb {
public:
    LegacyReverb() = default;
    void prepare(float sampleRate);
    // Original 0x25c10 clears delay memory but retains positions and damping
    // histories. Reconstruct the object for a genuinely fresh initial state.
    void reset() noexcept;
    // Native lifecycle helper: clear all signal history and cursors without
    // allocating or changing controls/lengths. Unlike reset(), this is fresh.
    void resetFresh() noexcept;
    // 0 dry, 1 wet, 2 equal-power mix, 3 mode (0 summed stereo, 1 independent),
    // 4/5 predelay L/R, 6/7 room L/R, 8/9 damp L/R, 10 common predelay,
    // 11 common room, 12 common damping, 13 width. Values are normalized.
    void configure(unsigned parameter, float value) noexcept;
    // Original additive processing, including its in-place dry compensation.
    // Input and output channels must have at least frames elements.
    void process(const float* const input[2], float* const output[2], int frames) noexcept;
    void process(float* left, float* right, int frames) noexcept;

private:
    struct Delay {
        std::vector<float> samples;
        int length = 0;
        int position = 0;
    };
    struct Comb : Delay { float history = 0.0f; };
    float predelay(unsigned channel, float input) noexcept;
    float network(unsigned channel, float input, float room, float damp) noexcept;
    void setPredelay(unsigned channel, int milliseconds) noexcept;
    std::array<float, 14> parameters_{
        0.8f, 0.2f, 0.0f, 0.0f, 0.1f, 0.1f, 0.7f,
        0.7f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f};
    std::array<Comb, 16> combs_{};
    std::array<Delay, 8> allpasses_{};
    std::array<Delay, 2> predelays_{};
    float sampleRate_ = 0.0f;
    bool ready_ = false;
};

} // namespace plasticz
