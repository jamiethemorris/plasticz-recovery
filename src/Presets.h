#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace plasticz {

inline constexpr std::size_t parameterCount = 38;
inline constexpr std::size_t programCount = 128;
inline constexpr std::uint32_t legacyPluginId = 0x504c435au; // PLCZ
using Parameters = std::array<float, parameterCount>;

struct Program {
    // Raw storage is preserved. Names are not necessarily null-terminated.
    std::array<char, 32> name {};
    Parameters values {};
};

using Bank = std::array<Program, programCount>;

Bank factoryBank();
const std::array<const char*, parameterCount>& parameterNames() noexcept;

struct LoadedState {
    Bank bank;
    int currentProgram = 0;
};

// Writes the recovered full-bank raw host chunk, with LE words and all names.
// Throws std::invalid_argument for invalid selection or non-normalized values.
std::vector<std::uint8_t> encodeState(const Bank&, int currentProgram);

// Raw chunk only: 1..128 programs, exactly 38 finite normalized parameters,
// LE/BE words, names present or absent. Missing slots/names use factory defaults.
// Failure leaves result unchanged. Current selection must be inside the chunk.
bool decodeState(const void*, std::size_t, LoadedState& result, std::string& error);

// Imports raw chunks or version-1 CcnK containers: FPCh/FBCh and FxCk/FxBk.
// Containers must identify PLCZ. Exact lengths are required; byteSize=0 is also
// accepted because the inspected JUCE writer emits zero. Missing raw slots/names
// retain current state. FxCk changes the selected slot; FxBk fills slots from 0
// while retaining current selection. FPCh applies the outer 28-byte name to the
// selection restored by its chunk, matching the inspected JUCE host behavior.
// Failure leaves result unchanged. This does not parse AU or DAW project files.
bool loadPresetFile(const void*, std::size_t, const LoadedState& current,
                    LoadedState& result, std::string& error);

} // namespace plasticz
