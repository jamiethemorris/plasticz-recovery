#include "Presets.h"
#include "FactoryPresets.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace plasticz {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Recovered state requires IEEE-754 binary32 floats");
constexpr std::size_t rawHeaderSize = 12;
constexpr std::size_t programValuesSize = parameterCount * 4;
constexpr std::size_t maxRawSize = rawHeaderSize + programCount * (programValuesSize + 32);
constexpr std::size_t fxProgramSize = 56 + programValuesSize;
constexpr std::size_t fxBankHeaderSize = 156;
constexpr std::size_t maxPresetSize = fxBankHeaderSize + programCount * fxProgramSize;

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

std::uint32_t word(const std::uint8_t* data, bool bigEndian) noexcept {
    if (bigEndian)
        return (std::uint32_t(data[0]) << 24) | (std::uint32_t(data[1]) << 16)
             | (std::uint32_t(data[2]) << 8) | std::uint32_t(data[3]);
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8)
         | (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

float asFloat(std::uint32_t bits) noexcept {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

std::uint32_t asWord(float value) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

bool normalized(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool validCurrent(int current) noexcept {
    return current >= 0 && current < static_cast<int>(programCount);
}

bool validateBank(const Bank& bank, std::string& error) {
    for (std::size_t p = 0; p < bank.size(); ++p)
        for (std::size_t i = 0; i < parameterCount; ++i)
            if (!normalized(bank[p].values[i]))
                return fail(error, "Program " + std::to_string(p) + ", parameter " + std::to_string(i)
                                   + " is not finite and within [0,1]");
    return true;
}

void appendLE(std::vector<std::uint8_t>& result, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        result.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool readParameters(const std::uint8_t* data, bool bigEndian, Parameters& result, std::string& error) {
    for (std::size_t index = 0; index < parameterCount; ++index) {
        const auto value = asFloat(word(data + index * 4, bigEndian));
        if (!normalized(value))
            return fail(error, "Parameter " + std::to_string(index) + " is not finite and within [0,1]");
        result[index] = value;
    }
    return true;
}

// Static evidence: state.rea.json getChunk 0x13204 and setChunk 0x1356e.
// This intentionally rejects malformed lengths the original setter ignored.
bool readRaw(const std::uint8_t* data, std::size_t size, const LoadedState& base,
             LoadedState& result, std::string& error) {
    if (data == nullptr || size < rawHeaderSize || size > maxRawSize)
        return fail(error, "Raw state size is outside the supported range");
    const auto bigEndian = (word(data, false) & 0xffff0000u) != 0;
    const auto count = word(data, bigEndian);
    const auto parameters = word(data + 4, bigEndian);
    const auto selected = word(data + 8, bigEndian);
    if (count == 0 || count > programCount)
        return fail(error, "Raw state must contain 1..128 programs");
    if (parameters != parameterCount)
        return fail(error, "Raw state must contain exactly 38 parameters per program");
    if (selected >= count)
        return fail(error, "Raw state current program is outside its supplied bank");
    const auto namesOffset = rawHeaderSize + count * programValuesSize;
    const bool hasNames = size == namesOffset + count * 32;
    if (size != namesOffset && !hasNames)
        return fail(error, "Raw state length must match all values and optional complete names exactly");
    auto candidate = base;
    for (std::size_t p = 0; p < count; ++p) {
        if (!readParameters(data + rawHeaderSize + p * programValuesSize, bigEndian,
                            candidate.bank[p].values, error))
            return false;
        if (hasNames)
            std::memcpy(candidate.bank[p].name.data(), data + namesOffset + p * 32, 32);
    }
    candidate.currentProgram = static_cast<int>(selected);
    result = std::move(candidate);
    return true;
}

bool magic(const std::uint8_t* data, const char* value) noexcept {
    return std::memcmp(data, value, 4) == 0;
}

// Source inspected read-only: JUCE 7.0.5 juce_VSTPluginFormat.cpp,
// structs at lines 95..147, loadFromFXBFile at 1763..1853, writer at 1855..1940.
// Outer numeric fields use big endian. Container payload words are independent.
bool containerHeader(const std::uint8_t* data, std::size_t size, std::string& error) {
    if (size < 28 || !magic(data, "CcnK"))
        return fail(error, "Invalid or truncated CcnK preset header");
    const auto declaredSize = word(data + 4, true);
    if (declaredSize != 0 && declaredSize != size - 8)
        return fail(error, "Preset byteSize does not match its exact file length");
    if (word(data + 12, true) != 1)
        return fail(error, "Only version-1 FXP/FXB containers are currently supported");
    if (word(data + 16, true) != legacyPluginId)
        return fail(error, "Preset belongs to another plug-in; expected PLCZ");
    return true;
}

void readName28(const std::uint8_t* data, std::array<char, 32>& name) noexcept {
    name.fill(0);
    std::memcpy(name.data(), data, 28);
}

bool readFxProgram(const std::uint8_t* data, std::size_t size, Program& result, std::string& error) {
    if (!containerHeader(data, size, error))
        return false;
    if (!magic(data + 8, "FxCk") || size != fxProgramSize || word(data + 24, true) != parameterCount)
        return fail(error, "FxCk must contain exactly 38 parameters and no trailing data");
    if (!readParameters(data + 56, true, result.values, error))
        return false;
    readName28(data + 28, result.name);
    return true;
}

} // namespace

Bank factoryBank() {
    Bank result;
    for (std::size_t p = 0; p < programCount; ++p) {
        result[p].name = detail::factoryProgramNames[p];
        for (std::size_t i = 0; i < parameterCount; ++i)
            result[p].values[i] = asFloat(detail::factoryParameterWords[p][i]);
    }
    return result;
}

const std::array<const char*, parameterCount>& parameterNames() noexcept {
    return detail::factoryParameterNames;
}

std::vector<std::uint8_t> encodeState(const Bank& bank, int currentProgram) {
    if (!validCurrent(currentProgram))
        throw std::invalid_argument("Current program must be in 0..127");
    std::string error;
    if (!validateBank(bank, error))
        throw std::invalid_argument(error);
    std::vector<std::uint8_t> result;
    result.reserve(maxRawSize);
    appendLE(result, static_cast<std::uint32_t>(programCount));
    appendLE(result, static_cast<std::uint32_t>(parameterCount));
    appendLE(result, static_cast<std::uint32_t>(currentProgram));
    for (const auto& program : bank)
        for (const auto value : program.values)
            appendLE(result, asWord(value));
    for (const auto& program : bank)
        for (const auto value : program.name)
            result.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
    return result;
}

bool decodeState(const void* data, std::size_t size, LoadedState& result, std::string& error) {
    const LoadedState base { factoryBank(), 0 };
    if (!readRaw(static_cast<const std::uint8_t*>(data), size, base, result, error))
        return false;
    error.clear();
    return true;
}

bool loadPresetFile(const void* input, std::size_t size, const LoadedState& current,
                    LoadedState& result, std::string& error) {
    if (!validCurrent(current.currentProgram))
        return fail(error, "Supplied current state has an invalid program selection");
    if (!validateBank(current.bank, error))
        return false;
    if (input == nullptr || size < 4 || size > maxPresetSize)
        return fail(error, "Preset size is outside the supported range");
    const auto* data = static_cast<const std::uint8_t*>(input);
    auto candidate = current;
    if (!magic(data, "CcnK")) {
        if (!readRaw(data, size, current, candidate, error))
            return false;
    } else {
        if (!containerHeader(data, size, error))
            return false;
        const auto count = word(data + 24, true);
        if (magic(data + 8, "FxCk")) {
            if (!readFxProgram(data, size, candidate.bank[static_cast<std::size_t>(current.currentProgram)], error))
                return false;
        } else if (magic(data + 8, "FxBk")) {
            if (count == 0 || count > programCount || size != fxBankHeaderSize + count * fxProgramSize)
                return fail(error, "FxBk count and length must describe 1..128 complete 38-parameter programs");
            for (std::size_t p = 0; p < count; ++p)
                if (!readFxProgram(data + fxBankHeaderSize + p * fxProgramSize,
                                   fxProgramSize, candidate.bank[p], error))
                    return false;
        } else if (magic(data + 8, "FPCh") || magic(data + 8, "FBCh")) {
            const bool isProgram = magic(data + 8, "FPCh");
            const std::size_t headerSize = isProgram ? 60 : 160;
            if (count == 0 || count > programCount || size < headerSize)
                return fail(error, "Invalid chunk-container program count or truncated header");
            const auto chunkSize = word(data + headerSize - 4, true);
            if (chunkSize != size - headerSize)
                return fail(error, "Container chunkSize must match its exact payload length");
            if (!readRaw(data + headerSize, chunkSize, current, candidate, error))
                return false;
            if (isProgram)
                readName28(data + 28, candidate.bank[static_cast<std::size_t>(candidate.currentProgram)].name);
        } else {
            return fail(error, "Unsupported preset container; expected FxCk, FxBk, FPCh, or FBCh");
        }
    }
    result = std::move(candidate);
    error.clear();
    return true;
}

} // namespace plasticz
