#include "SynthEngine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace plasticz {
namespace {
float normalized(float x) noexcept {
    return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.0f;
}
// 0x10617..0x10635: preserve SSE operation order and constant bit patterns.
float envelopeIncrement(float parameter, float offset = 0.10773000121116638f) noexcept {
    float x = parameter * 0.8922699689865112f + offset;
    float square = x * x;
    float cube = x * square;
    return (1.0f / cube) * 0.0002500000118743628f;
}
float stepToward(float current, float target, float amount) noexcept {
    return target < current ? std::max(target, current - amount)
                            : std::min(target, current + amount);
}
}

void SynthEngine::prepare(double sampleRate, int) {
    if (!std::isfinite(sampleRate)) sampleRate = 44100.0;
    sampleRate_ = static_cast<float>(std::clamp(sampleRate, 8000.0, 384000.0));
    // Original setter at 0x12c9e; don't algebraically reassociate the division.
    rateFactor_ = (1.0f / sampleRate_) * 44100.0f;
    effects_.prepare(sampleRate_);
    output_.prepare(sampleRate_);
    reset();
}

void SynthEngine::reset() noexcept {
    voices_.fill({});
    heldNotes_.fill(0);
    heldCount_ = 0;
    // The original starts in poly mode and performs mono cleanup on its first
    // control tick. Keep that initial state observable to the binary oracle.
    mono_ = false;
    controlCountdown_ = 0;
    bend_ = bendTarget_ = 0.5f;
    wheel_ = vibratoPhase_ = 0;
    volume_ = expression_ = 1;
    effects_.reset();
    output_.reset();
    downsampler_.reset();
}

void SynthEngine::setParameters(const Parameters& values) noexcept {
    for (std::size_t i = 0; i < parameters_.size(); ++i)
        parameters_[i] = normalized(values[i]);
    effects_.setParameters(parameters_);
    updateMonoMode();
}

void SynthEngine::updateMonoMode() noexcept {
    const bool nextMono = static_cast<int>(parameters_[35] * 15.989999771118164f) == 0;
    if (nextMono != mono_ && nextMono) {
        for (auto& v : voices_) {
            v.held = v.ampStage = 0;
            v.ampLevel = 0;
            v.dcwStage = 1;
            v.dcwLevel = 0;
        }
        heldCount_ = 0;
    }
    mono_ = nextMono;
}

void SynthEngine::glide(VoiceState& v, int note) noexcept {
    constexpr std::array<float, 7> divisions{240, 120, 60, 30, 15, 7.5f, 3.75f};
    v.note = static_cast<std::uint8_t>(note);
    v.targetNote = static_cast<float>(note);
    const auto index = static_cast<std::size_t>(parameters_[33] * 6.989999771118164f);
    v.glideStep = std::abs(((v.targetNote - v.currentNote) / (1000.0f * divisions[index])) * tempo_);
}

void SynthEngine::noteOn(int note, float velocity) noexcept {
    if (note < 0 || note > 127) return;
    // Hosts may deliver a note before the first control tick after preparation.
    updateMonoMode();
    velocity = normalized(velocity);
    if (velocity <= 0) { noteOff(note); return; }
    if (mono_) {
        const int previousCount = heldCount_;
        // Bounded replacement for the legacy fixed-array overflow on repeated MIDI.
        heldCount_ = std::min(heldCount_ + 1, 128);
        for (int i = heldCount_ - 1; i > 0; --i) heldNotes_[i] = heldNotes_[i - 1];
        heldNotes_[0] = static_cast<std::uint8_t>(note);
        if (previousCount != 0) {
            for (auto& v : voices_) if (v.held) { glide(v, note); return; }
            return;
        }
    }
    // sub_f5e4: first unheld candidate, then original stage/progress ordering.
    const int count = std::max(2, static_cast<int>(parameters_[35] * 15.989999771118164f) + 1);
    int selected = 0, scan = 1;
    if (voices_[0].held) {
        for (int i = 1; i < count; ++i) {
            if (!voices_[i].held) { selected = i; scan = i + 1; break; }
        }
    }
    for (int i = scan; i < count; ++i) {
        if (voices_[i].ampStage < voices_[selected].ampStage
            || voices_[i].ampProgress > voices_[selected].ampProgress)
            selected = i;
    }
    if (mono_) {
        for (int i = 0; i < 16; ++i) if (i != selected) {
            auto& v = voices_[i];
            v.held = v.ampStage = 0;
            v.ampLevel = 0;
            v.dcwStage = 1;
            v.dcwLevel = 0;
        }
    }
    auto& v = voices_[selected];
    v = {};
    // Original 0xf6fd resets the pattern clock only when allocating a voice;
    // mono legato returns above, and the smoothed gate amplitude is retained.
    effects_.retriggerGate();
    v.note = static_cast<std::uint8_t>(note);
    v.velocity = velocity;
    v.held = 1;
    v.ampStage = v.dcwStage = 4;
    v.currentNote = static_cast<float>(note);
}

void SynthEngine::release(VoiceState& v) noexcept {
    v.held = 0;
    v.ampStage = v.dcwStage = 1;
    v.ampAtRelease = v.ampLevel;
    v.dcwAtRelease = v.dcwLevel;
    v.ampProgress = 0;
}

void SynthEngine::noteOff(int note) noexcept {
    if (note < 0 || note > 127) return;
    if (mono_) {
        for (int i = 0; i < heldCount_; ++i) if (heldNotes_[i] == note) {
            for (int j = i; j + 1 < heldCount_; ++j) heldNotes_[j] = heldNotes_[j + 1];
            --heldCount_;
            break;
        }
        if (heldCount_ > 0) {
            for (auto& v : voices_) if (v.held) { glide(v, heldNotes_[0]); return; }
            return;
        }
    }
    for (auto& v : voices_) if (v.held && v.note == note) { release(v); return; }
}

void SynthEngine::pitchBend(float value) noexcept { bendTarget_ = normalized(value); }
void SynthEngine::modWheel(float value) noexcept { wheel_ = normalized(value); }
void SynthEngine::setVolume(float value) noexcept { volume_ = normalized(value); }
void SynthEngine::setExpression(float value) noexcept { expression_ = normalized(value); }
void SynthEngine::setTempo(double value) noexcept {
    if (std::isfinite(value) && value > 0) {
        tempo_ = static_cast<float>(value);
        effects_.setTempo(tempo_);
    }
}

void SynthEngine::allNotesOff(bool immediate) noexcept {
    heldCount_ = 0;
    for (auto& v : voices_) {
        if (immediate) v = {};
        else if (v.held) release(v);
    }
    if (immediate) {
        effects_.reset();
        output_.reset();
        downsampler_.reset();
    }
}

void SynthEngine::controlTick() noexcept {
    // Direct reconstruction of sub_10534 (including tails through 0x10bb6).
    updateMonoMode();
    const auto& p = parameters_;
    bend_ = stepToward(bend_, bendTarget_, 0.009999999776482582f);
    float bendRange = static_cast<float>(static_cast<int>(11.999999046325684f * p[32])) + 1.0f;
    float bendSemitones = (bend_ + bend_ - 1.0f) * bendRange;
    const float aa = envelopeIncrement(p[20]);
    const float ad = envelopeIncrement(p[21]);
    const float ar = envelopeIncrement(p[23]);
    const float da = envelopeIncrement(p[0]);
    const float dd = envelopeIncrement(p[1]);
    const float dr = envelopeIncrement(p[3], 0.2077299952507019f);
    vibratoPhase_ += (19.0f * p[34] + 1.0f) * 0.0010000000474974513f;
    vibratoPhase_ -= static_cast<float>(static_cast<int>(vibratoPhase_));
    float x = vibratoPhase_ - static_cast<float>(static_cast<int>(vibratoPhase_));
    float modulation;
    if (x < 0.5f) modulation = (x * x) * -16.0f + x * 8.0f;
    else { x -= 0.5f; modulation = (x * x) * 16.0f + x * -8.0f; }
    modulation *= wheel_;

    for (auto& v : voices_) {
        switch (v.ampStage) {
            case 4:
                v.ampProgress += aa;
                if (v.ampProgress >= 1) { v.ampProgress = 0; v.ampStage = 3; v.ampLevel = 1; }
                else v.ampLevel = v.ampProgress;
                break;
            case 3:
                v.ampProgress += ad;
                if (v.ampProgress >= 1) { v.ampProgress = 0; v.ampStage = 2; v.ampLevel = p[22]; }
                else v.ampLevel = v.ampProgress * p[22] + (1.0f - v.ampProgress);
                break;
            case 2: v.ampLevel = p[22]; break;
            case 1:
                v.ampProgress += ar;
                if (v.ampProgress >= 1) { v.ampStage = 0; v.ampLevel = 0; }
                else v.ampLevel = (1.0f - v.ampProgress) * v.ampAtRelease;
                break;
            default: break;
        }
        switch (v.dcwStage) {
            case 4:
                v.dcwProgress += da;
                if (v.dcwProgress >= 1) { v.dcwProgress = 0; v.dcwStage = 3; v.dcwLevel = 1; }
                else v.dcwLevel = v.dcwProgress;
                break;
            case 3:
                v.dcwProgress += dd;
                if (v.dcwProgress >= 1) { v.dcwProgress = 0; v.dcwStage = 2; v.dcwLevel = p[2]; }
                else v.dcwLevel = v.dcwProgress * p[2] + (1.0f - v.dcwProgress);
                break;
            case 2: v.dcwLevel = p[2]; break;
            case 1: if (v.dcwLevel > 0) v.dcwLevel -= dr; break;
            default: break;
        }
        if (mono_) v.currentNote = stepToward(v.currentNote, v.targetNote, v.glideStep);
        else v.currentNote = static_cast<float>(v.note);
        float pitch = (modulation + v.currentNote + bendSemitones) * 0.0577622652053833f;
        v.phaseStep = ((std::exp(pitch) * 8.175749778747559f) * rateFactor_) * 5.668934136338066e-06f;
        float velocityGain = p[25] * v.velocity + (1.0f - p[25]);
        float curve = ((v.ampLevel - 1.0f) * 6.0f) / ((1.0f - v.ampLevel) * 5.0f + 1.0f) + 1.0f;
        v.ampTarget = ((velocityGain * (curve * p[24])) * volume_) * expression_;
        if (!(v.ampTarget > 3.814697265625e-06f)) v.ampTarget = 0;
        float dcwVelocity = (1.0f - p[5]) + std::pow(v.velocity, 0.25f) * p[5];
        v.dcwTarget = (p[4] * v.dcwLevel) * dcwVelocity;
    }
}

void SynthEngine::renderDry(float* mono, int samples) noexcept {
    if (!mono || samples <= 0) return;
    std::uint16_t activeVoices = 0;
    for (std::size_t i = 0; i < voices_.size(); ++i)
        if (voices_[i].ampStage || voices_[i].smoothAmp > 3.0517578125e-05f)
            activeVoices |= static_cast<std::uint16_t>(1u << i);
    if (parameters_[37] < 0.5f) {
        renderVoices(mono, samples, false, activeVoices);
        return;
    }
    // Original 0xffeb: if all voices are inactive, leave the output and filter
    // histories alone. Switching 4x off likewise preserves its history.
    if (!activeVoices) return;
    int offset = 0;
    while (offset < samples) {
        const int count = std::min(samples - offset, static_cast<int>(oversampled_.size() / 4));
        std::fill_n(oversampled_.data(), count * 4, 0.0f);
        renderVoices(oversampled_.data(), count * 4, true, activeVoices);
        downsampler_.process(oversampled_.data(), mono + offset, count);
        offset += count;
    }
    downsampler_.finishBlock();
}

void SynthEngine::renderVoices(float* mono, int samples, bool oversampled, std::uint16_t activeVoices) noexcept {
    const auto& p = parameters_;
    std::array<Waveform, 4> waves;
    for (int i = 0; i < 4; ++i) waves[i] = static_cast<Waveform>(static_cast<int>(p[8 + i] * 7.989999771118164f));
    const float rate = oversampled ? 0.25f * rateFactor_ : rateFactor_;
    const float ampSmooth = rate * 0.007499999832361937f;
    const float dcwSmooth = rate * 0.10000000149011612f;
    for (std::size_t voice = 0; voice < voices_.size(); ++voice) {
        if (!(activeVoices & (1u << voice))) continue;
        auto& v = voices_[voice];
        float tune1 = static_cast<float>(static_cast<int>(p[12] * 4.989999771118164f + 1.0f)) * v.phaseStep;
        tune1 = ((p[13] - 0.5f) * 0.059459999203681946f + 1.0f) * tune1;
        float tune2 = static_cast<float>(static_cast<int>(p[14] * 4.989999771118164f + 1.0f)) * v.phaseStep;
        tune2 = ((p[15] - 0.5f) * 0.059459999203681946f + 1.0f) * tune2;
        if (oversampled) {
            tune1 = tune1 * 0.25f;
            tune2 = 0.25f * tune2;
        }
        for (int i = 0; i < samples; ++i) {
            v.smoothDcw = (v.dcwTarget - v.smoothDcw) * dcwSmooth + v.smoothDcw;
            const float d = v.smoothDcw;
            float osc1 = v.phase1 < 0.5f ? waveforms_.evaluate(waves[0], v.phase1 + v.phase1, d)
                : waveforms_.evaluate(waves[1], (v.phase1 - 0.5f) * 2.0f, d);
            float osc2 = v.phase2 < 0.5f ? waveforms_.evaluate(waves[2], v.phase2 + v.phase2, d)
                : waveforms_.evaluate(waves[3], (v.phase2 - 0.5f) * 2.0f, d);
            osc1 = (Waveforms::dcOffset(waves[0], d) + Waveforms::dcOffset(waves[1], d)) * 0.5f + osc1;
            osc2 = (Waveforms::dcOffset(waves[3], d) + Waveforms::dcOffset(waves[2], d)) * 0.5f + osc2;
            v.smoothAmp = (v.ampTarget - v.smoothAmp) * ampSmooth + v.smoothAmp;
            float mix = osc1 * p[17] + osc2 * p[18];
            mix = mix + (osc1 * osc2) * p[16];
            mono[i] += (mix * v.smoothAmp) * 0.4000000059604645f;
            v.phase1 += tune1;
            v.phase1 -= static_cast<float>(static_cast<int>(v.phase1));
            v.phase2 += tune2;
            v.phase2 -= static_cast<float>(static_cast<int>(v.phase2));
        }
    }
}

void SynthEngine::render(float* left, float* right, int samples) noexcept {
    if (!left || !right || samples <= 0) return;
    std::fill_n(left, samples, 0.0f);
    std::fill_n(right, samples, 0.0f);
    int offset = 0;
    // Preserve the fractional 1 ms clock across host blocks and MIDI splits.
    while (offset < samples) {
        const int untilTick = static_cast<int>(controlCountdown_);
        const int count = std::min(untilTick, samples - offset);
        renderDry(left + offset, count);
        if (fullProcessing_ && count > 0) {
            output_.processInput(left + offset, count);
            effects_.process(left + offset, right + offset, count);
            output_.processOutput(left + offset, right + offset, count);
        }
        offset += count;
        controlCountdown_ -= static_cast<float>(count);
        if (count == untilTick) {
            controlCountdown_ += 0.0010000000474974513f * sampleRate_;
            controlTick();
        }
    }
    if (!fullProcessing_) std::copy_n(left, samples, right);
}
} // namespace plasticz
