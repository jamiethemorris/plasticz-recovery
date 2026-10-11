#include "Waveforms.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace plasticz {
namespace {
constexpr int mask = 0x3fff;
constexpr int half = 0x2000;

void checkDomain(float value, float minimum = 0.0f) {
    if (!std::isfinite(value) || value < minimum || value > 1.0f)
        throw std::domain_error("Value outside recovered waveform domain");
}

int truncInt(float value) { return static_cast<int>(value); }
}

Waveforms::Waveforms() {
    // sub_ed14, constants at 0x3f5b8/0x3f5bc. The historical Intel cosf
    // implementation is unavailable; a supplied table isolates this difference.
    for (std::size_t i = 0; i < table_.size(); ++i) {
        float angle = static_cast<float>(i) * 0.0003834952076431364f;
        angle = angle - 3.1415927410125732f;
        table_[i] = std::cos(angle);
    }
}

Waveforms::Waveforms(Table table) : table_(std::move(table)) {}

float Waveforms::evaluate(Waveform wave, float phase, float distortion) const {
    checkDomain(phase);
    // The original DCW release subtracts past zero once, then stops. Preserve
    // that small negative tail instead of clamping and changing release sound.
    checkDomain(distortion, -1.0f);
    // processReplacing sets the x87 control word to truncate (sub_2b980,
    // caller 0x138ae..0x138c9); nearest rounding changes audible lookup indices.
    const int p = truncInt(phase * 16384.0f) & mask;
    int index = p;
    switch (wave) {
        case Waveform::Saw: { // 0xef38
            const int knee = half - truncInt(distortion * 8191.0f);
            index = p <= knee ? (p << 13) / knee
                              : ((p ^ mask) << 13) / (knee ^ mask);
            break;
        }
        case Waveform::Square: { // 0xefbe
            const int knee = half - truncInt(distortion * 8191.0f);
            if (p <= half)
                index = p <= knee ? (p << 13) / knee : 0x1fff;
            else {
                const int q = p - half;
                index = knee <= q ? mask : (q << 13) / knee + half;
            }
            break;
        }
        case Waveform::Pulse: // 0xf072
        case Waveform::DoubleCosine: { // 0xf182 (Hopper splits body at 0xf185)
            const int knee = half - truncInt(distortion * 8150.0f);
            const int shift = wave == Waveform::Pulse ? 13 : 14;
            index = p <= knee * 2 ? ((p << shift) / knee) & mask : 0x3ffe;
            break;
        }
        case Waveform::SawPulse: { // 0xf0fa
            const int knee = half - truncInt(distortion * 8191.0f);
            if (p > half) {
                const int q = p - half;
                index = knee > q ? (q << 13) / knee + half : mask;
            }
            break;
        }
        case Waveform::Resonance: // 0xf20a
        case Waveform::ResonanceSquare: { // 0xf2a0
            const int knee = half - truncInt(distortion * 8100.0f);
            index = ((p << 13) / knee) & mask;
            const float carrier = wave == Waveform::Resonance
                                      ? table_[static_cast<std::size_t>(index)]
                                      : (index <= 0x1fff ? -1.0f : 1.0f);
            float value = static_cast<float>(p ^ mask) * carrier;
            value = value - static_cast<float>(p);
            return value * 0.00006103515625f;
        }
        case Waveform::DoubleSaw: { // 0xf34a
            const float scaled = distortion * 6112.0f;
            const int shift = truncInt(scaled);
            const int knee = half - shift;
            index = p <= knee * 2 ? ((p << 14) / knee) & mask : 0x3ffe;
            index = (index - shift) & mask;
            return static_cast<float>(index) * 0.0001220703125f - 1.0f;
        }
        default:
            throw std::invalid_argument("Unknown waveform");
    }
    return table_[static_cast<std::size_t>(index)];
}

float Waveforms::dcOffset(Waveform wave, float distortion) {
    checkDomain(distortion, -1.0f);
    // Function pointers at 0x4b120; waveform pointers at 0x4b140.
    switch (wave) {
        case Waveform::Saw:             return 0.0f; // 0xee9a
        case Waveform::Square:          return 0.0f; // 0xeea2
        case Waveform::Pulse:           return distortion; // 0xeeaa
        case Waveform::SawPulse:        return 0.5f * distortion; // 0xeeb2
        case Waveform::DoubleCosine:    return distortion; // 0xeed4
        case Waveform::Resonance:       return 0.5f; // 0xeefe
        case Waveform::ResonanceSquare: return -0.3f * distortion + 0.7f; // 0xef0e
        case Waveform::DoubleSaw:       return -0.38f * distortion; // 0xeedc
    }
    throw std::invalid_argument("Unknown waveform");
}
} // namespace plasticz
