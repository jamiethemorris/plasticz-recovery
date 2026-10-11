#pragma once
#include "Presets.h"
#include "LegacyReverb.h"
#include <array>
#include <cstdint>
#include <vector>

namespace plasticz {
// Native reconstruction of 0x10e4a's effect cases and helper 0x10bbc.
// Input filtering and final dynamics are performed by OutputProcessing.
class Effects {
public:
    void prepare(float sampleRate);
    void reset() noexcept;
    void setParameters(const Parameters&) noexcept;
    void setTempo(float bpm) noexcept { tempo_ = bpm; }
    void retriggerGate() noexcept { s(0x4005f0) = 0.0f; }
    void process(float* left, float* right, int count) noexcept;
private:
    void resetEffectState() noexcept;
    float quadrature(float input, float phase) noexcept;
    float& s(unsigned address) noexcept { return state_[(address - 0x4005d0) / 4]; }
    float tap(unsigned channel, std::uint32_t index) const noexcept {
        return delay_[channel * 0x40000u + index];
    }
    float& tap(unsigned channel, std::uint32_t index) noexcept {
        return delay_[channel * 0x40000u + index];
    }
    float interpolated(unsigned channel, float offset, unsigned extra = 0) noexcept;
    std::vector<float> delay_;
    // Original scalar addresses retained here to make assembly comparisons direct.
    std::array<float, 86> state_{};
    std::array<std::uint32_t, 3> position_{};
    unsigned crusherCount_{};
    LegacyReverb reverb_;
    Parameters parameters_{};
    float sampleRate_{44100.0f}, tempo_{120.0f};
    int type_{-1};
    bool clearDelayOnTypeChange_{true};
};
} // namespace plasticz
