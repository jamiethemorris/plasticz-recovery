#pragma once

#include "Presets.h"
#include "Waveforms.h"
#include "Effects.h"
#include "OutputProcessing.h"
#include "Downsampler4x.h"
#include <array>
#include <cstdint>

namespace plasticz {

// Layout mirrors the recovered 0x5c-byte record, for differential state tests.
struct VoiceState {
    std::uint8_t note{}, padding0[3]{};
    float velocity{};
    float dcwLevel{}, dcwProgress{}, dcwAtRelease{};
    std::uint8_t dcwStage{}, padding1[3]{};
    float ampLevel{}, ampProgress{}, ampAtRelease{};
    std::uint8_t ampStage{}, held{}, padding2[2]{};
    float smoothAmp{}, ampTarget{}, unused30{}, unused34{};
    float dcwTarget{}, smoothDcw{}, phase1{}, phase2{}, phaseStep{}, unused4c{};
    float targetNote{}, currentNote{}, glideStep{};
};
static_assert(sizeof(VoiceState) == 0x5c);

// Native synthesis, original 4x decimation, and effect/output path.
// No allocation, locks, or emulation occurs in the processing methods.
class SynthEngine {
public:
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParameters(const Parameters&) noexcept;
    void noteOn(int note, float velocity) noexcept;
    void noteOff(int note) noexcept;
    void pitchBend(float normalized) noexcept;
    void modWheel(float normalized) noexcept;
    void setTempo(double bpm) noexcept;
    void setVolume(float value) noexcept;
    void setExpression(float value) noexcept;
    void allNotesOff(bool immediate) noexcept;
    void render(float* left, float* right, int samples) noexcept;
    void setFullProcessingEnabled(bool enabled) noexcept { fullProcessing_ = enabled; }
    int latencySamples() const noexcept { return fullProcessing_ ? output_.latencySamples() : 0; }

    // Exposed deterministic building blocks for original-instruction comparisons.
    void controlTick() noexcept;
    void renderDry(float* mono, int samples) noexcept;
    const std::array<VoiceState, 16>& voiceStates() const noexcept { return voices_; }
    const std::array<float, 10>& downsamplerState() const noexcept { return downsampler_.history(); }

private:
    void updateMonoMode() noexcept;
    void glide(VoiceState&, int note) noexcept;
    static void release(VoiceState&) noexcept;
    void renderVoices(float* mono, int samples, bool oversampled, std::uint16_t activeVoices) noexcept;

    Waveforms waveforms_;
    Effects effects_;
    OutputProcessing output_;
    Downsampler4x downsampler_;
    // Fixed scratch storage also supports direct renderDry calls larger than
    // the host's preparation size. Chunking never introduces extra cleanup.
    std::array<float, 4096> oversampled_{};
    Parameters parameters_{};
    std::array<VoiceState, 16> voices_{};
    std::array<std::uint8_t, 128> heldNotes_{};
    int heldCount_{};
    bool mono_{};
    float sampleRate_{44100.0f}, rateFactor_{1.0f}, controlCountdown_{};
    float bend_{0.5f}, bendTarget_{0.5f}, wheel_{}, vibratoPhase_{};
    float volume_{1.0f}, expression_{1.0f}, tempo_{120.0f};
    bool fullProcessing_{true};
};
} // namespace plasticz
