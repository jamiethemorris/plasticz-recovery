#pragma once

#include <array>
#include <cstddef>

namespace plasticz {

// Original host order, recovered from the function and parameter display tables.
enum class Waveform { Saw, Square, Pulse, SawPulse, DoubleCosine, Resonance,
                      ResonanceSquare, DoubleSaw };

class Waveforms {
public:
    static constexpr std::size_t tableSize = 16384;
    using Table = std::array<float, tableSize>;

    Waveforms();
    explicit Waveforms(Table table);

    // Finite phase in [0, 1], distortion in [-1, 1]. The negative range permits
    // the original DCW release's single-step undershoot; controls stay [0, 1].
    // This is the recovered scalar kernel, not a complete synthesizer or plug-in.
    float evaluate(Waveform wave, float phase, float distortion) const;
    static float dcOffset(Waveform wave, float distortion);
    const Table& cosineTable() const noexcept { return table_; }

private:
    Table table_;
};

} // namespace plasticz
