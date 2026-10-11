#include "Presets.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using namespace plasticz;

void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::uint32_t bits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}

float fromBits(std::uint32_t value) {
    float result;
    std::memcpy(&result, &value, 4);
    return result;
}

void putWord(Bytes& bytes, std::size_t offset, std::uint32_t value, bool big = true) {
    check(offset + 4 <= bytes.size(), "Fixture write beyond bounds");
    for (std::size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (big ? 24 - 8 * i : 8 * i));
}

void putMagic(Bytes& bytes, std::size_t offset, const char* value) {
    std::memcpy(bytes.data() + offset, value, 4);
}

std::string name(const Program& program) {
    const auto end = std::find(program.name.begin(), program.name.end(), '\0');
    return { program.name.begin(), end };
}

bool sameProgram(const Program& left, const Program& right) {
    return left.name == right.name && std::memcmp(left.values.data(), right.values.data(), 38 * 4) == 0;
}

bool sameState(const LoadedState& left, const LoadedState& right) {
    if (left.currentProgram != right.currentProgram)
        return false;
    for (std::size_t p = 0; p < programCount; ++p)
        if (!sameProgram(left.bank[p], right.bank[p]))
            return false;
    return true;
}

// Independent on-disk fixture: all floats first, then optional raw names.
Bytes rawFixture(bool big, bool names = true, std::uint32_t count = 2) {
    Bytes bytes(12 + count * (38 * 4 + (names ? 32 : 0)), 0);
    putWord(bytes, 0, count, big);
    putWord(bytes, 4, 38, big);
    putWord(bytes, 8, count - 1, big);
    for (std::size_t p = 0; p < count; ++p)
        for (std::size_t i = 0; i < 38; ++i) {
            auto value = p == 0 ? 0x3e800000u : 0x3f000000u;
            if (p == 0 && i == 0) value = 0x80000000u; // negative zero
            if (p == 0 && i == 1) value = 0x00000001u; // smallest positive subnormal
            if (p == 0 && i == 2) value = 0x3f800000u;
            putWord(bytes, 12 + (p * 38 + i) * 4, value, big);
        }
    if (names)
        for (std::size_t p = 0; p < count; ++p)
            for (std::size_t i = 0; i < 32; ++i)
                bytes[12 + count * 38 * 4 + p * 32 + i] = static_cast<std::uint8_t>(128 + p * 32 + i);
    return bytes;
}

Bytes container(const char* kind, std::size_t length, std::uint32_t count) {
    Bytes bytes(length, 0);
    putMagic(bytes, 0, "CcnK");
    putWord(bytes, 4, static_cast<std::uint32_t>(length - 8));
    putMagic(bytes, 8, kind);
    putWord(bytes, 12, 1);
    putWord(bytes, 16, 0x504c435a);
    putWord(bytes, 20, 100); // Plug-in version is metadata, not container version.
    putWord(bytes, 24, count);
    return bytes;
}

Bytes fxProgram(std::uint32_t parameterBits = 0x3f000000u, char character = 'A') {
    auto bytes = container("FxCk", 56 + 38 * 4, 38);
    std::fill(bytes.begin() + 28, bytes.begin() + 56, static_cast<std::uint8_t>(character));
    for (std::size_t i = 0; i < 38; ++i)
        putWord(bytes, 56 + i * 4, parameterBits);
    return bytes;
}

Bytes fxBank() {
    auto bytes = container("FxBk", 156 + 2 * (56 + 38 * 4), 2);
    const auto first = fxProgram(0x3e800000u, 'A');
    const auto second = fxProgram(0x3f000000u, 'B');
    std::copy(first.begin(), first.end(), bytes.begin() + 156);
    std::copy(second.begin(), second.end(), bytes.begin() + 156 + first.size());
    return bytes;
}

Bytes chunkContainer(bool program, bool bigRaw) {
    const auto raw = rawFixture(bigRaw);
    const std::size_t header = program ? 60 : 160;
    // JUCE may put the full bank's program count in an FPCh wrapper.
    auto bytes = container(program ? "FPCh" : "FBCh", header + raw.size(), program ? 128 : 2);
    if (program)
        std::fill(bytes.begin() + 28, bytes.begin() + 56, 'P');
    putWord(bytes, header - 4, static_cast<std::uint32_t>(raw.size()));
    std::copy(raw.begin(), raw.end(), bytes.begin() + header);
    return bytes;
}

void rawMustFail(const Bytes& bytes, const LoadedState& sentinel) {
    auto result = sentinel;
    std::string error;
    check(!decodeState(bytes.data(), bytes.size(), result, error), "Malformed raw chunk was accepted");
    check(!error.empty(), "Raw decode failure needs an error message");
    check(sameState(result, sentinel), "Raw decode failure modified output");
}

void fileMustFail(const Bytes& bytes, const LoadedState& sentinel) {
    auto result = sentinel;
    std::string error;
    check(!loadPresetFile(bytes.data(), bytes.size(), sentinel, result, error), "Malformed preset was accepted");
    check(!error.empty(), "Import failure needs an error message");
    check(sameState(result, sentinel), "Import failure modified output");
}

void testFactory(const char* goldenFile) {
    LoadedState factory { factoryBank(), 0 };
    check(name(factory.bank[0]) == "BA Fat Synth HM", "First factory name");
    check(name(factory.bank[127]) == "FX Female Vox", "Last factory name");
    check(name(factory.bank[70]) == "ORG Funk ", "Factory trailing space preserved");
    check(std::string(parameterNames()[0]) == "DCW_ATK" && std::string(parameterNames()[6]) == "dummy1"
          && std::string(parameterNames()[37]) == "4X OVER", "Original host labels preserved");
    const auto bytes = encodeState(factory.bank, factory.currentProgram);
    check(bytes.size() == 23564, "Full-bank raw size");
    std::ifstream stream(goldenFile, std::ios::binary);
    check(bool(stream), std::string("Cannot read independently recovered factory chunk: ") + goldenFile);
    const Bytes golden { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    check(bytes == golden, "Every factory float word and raw name must match Python-extracted binary evidence");
    LoadedState decoded {};
    std::string error = "stale";
    check(decodeState(bytes.data(), bytes.size(), decoded, error), "Factory chunk decode");
    check(error.empty() && sameState(decoded, factory), "Factory bit-exact round trip");

    factory.bank[0].values[0] = fromBits(0x80000000u);
    factory.bank[0].values[1] = fromBits(1);
    factory.bank[127].name[31] = static_cast<char>(0xff);
    factory.currentProgram = 127;
    const auto special = encodeState(factory.bank, factory.currentProgram);
    check(decodeState(special.data(), special.size(), decoded, error), "Special float words decode");
    check(sameState(decoded, factory), "Negative zero, subnormal, and opaque name bytes round trip");
    for (const auto index : { -1, 128 }) {
        bool threw = false;
        try { (void)encodeState(factory.bank, index); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "Invalid encoded selection must throw");
    }
    factory.bank[0].values[0] = std::numeric_limits<float>::infinity();
    bool threw = false;
    try { (void)encodeState(factory.bank, 0); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "Encoding invalid values must throw");
}

void testRaw() {
    LoadedState current { factoryBank(), 42 };
    current.bank[0].name.fill('Z');
    current.bank[100].values[0] = 0.25f;
    for (const bool big : { false, true })
        for (const bool hasNames : { false, true })
            for (const auto count : { 1u, 2u }) {
                const auto bytes = rawFixture(big, hasNames, count);
                LoadedState decoded {};
                std::string error;
                check(decodeState(bytes.data(), bytes.size(), decoded, error), "Independent raw fixture decode");
                check(bits(decoded.bank[0].values[0]) == 0x80000000u && bits(decoded.bank[0].values[1]) == 1u,
                      "Raw special float bits preserved");
                check(decoded.currentProgram == static_cast<int>(count - 1), "Raw selected program");
                check(sameProgram(decoded.bank[100], factoryBank()[100]), "decodeState fills missing slots from factory");
                if (hasNames)
                    check(static_cast<unsigned char>(decoded.bank[0].name[31]) == 159, "Raw names are not byte-swapped or truncated");
                else
                    check(decoded.bank[0].name == factoryBank()[0].name, "decodeState preserves factory names if absent");
                check(loadPresetFile(bytes.data(), bytes.size(), current, decoded, error), "Raw file import");
                check(sameProgram(decoded.bank[100], current.bank[100]), "Raw import fills missing slots from current");
                if (!hasNames)
                    check(decoded.bank[0].name == current.bank[0].name, "Raw import preserves current names if absent");
            }
    for (const bool big : { false, true }) {
        for (const auto invalid : { 0x7fc00000u, 0x7f800001u, 0x7f800000u, 0xff800000u, 0xbf000000u, 0x3f800001u }) {
            auto bytes = rawFixture(big);
            putWord(bytes, 12 + 37 * 4, invalid, big);
            rawMustFail(bytes, current);
        }
        for (const auto header : { std::array<std::uint32_t, 3>{ 0, 38, 0 }, { 129, 38, 0 },
                                   { 2, 37, 0 }, { 2, 39, 0 }, { 2, 38, 2 }, { 2, 38, 0xffffffffu } }) {
            auto bytes = rawFixture(big);
            for (std::size_t i = 0; i < 3; ++i) putWord(bytes, i * 4, header[i], big);
            rawMustFail(bytes, current);
        }
    }
    const auto full = rawFixture(false);
    for (std::size_t size = 0; size < full.size(); ++size) {
        if (size == 12 + 2 * 38 * 4) continue; // The complete nameless variant is valid.
        rawMustFail(Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(size)), current);
    }
    auto trailing = full;
    trailing.push_back(0);
    rawMustFail(trailing, current);
    LoadedState result = current;
    std::string error;
    check(!decodeState(nullptr, 196, result, error) && sameState(current, result), "Null raw input rejected transactionally");
}

void testContainers() {
    LoadedState current { factoryBank(), 42 }, result {};
    current.bank[100].values[0] = 0.25f;
    std::string error;
    auto program = fxProgram(0x80000000u);
    check(loadPresetFile(program.data(), program.size(), current, result, error), "Standard FxCk import");
    check(result.currentProgram == 42 && bits(result.bank[42].values[37]) == 0x80000000u,
          "FxCk updates selected slot and preserves negative zero");
    check(result.bank[42].name[27] == 'A' && result.bank[42].name[28] == 0, "28-byte preset name storage");
    check(sameProgram(result.bank[0], current.bank[0]), "FxCk leaves other slots intact");
    putWord(program, 4, 0); // Verified JUCE writer convention.
    check(loadPresetFile(program.data(), program.size(), current, result, error), "JUCE zero byteSize accepted");

    const auto bank = fxBank();
    check(loadPresetFile(bank.data(), bank.size(), current, result, error), "Standard FxBk import");
    check(result.currentProgram == 42 && result.bank[0].values[37] == 0.25f && result.bank[1].values[37] == 0.5f,
          "FxBk program order and original selected slot");
    check(sameProgram(result.bank[100], current.bank[100]), "FxBk partial bank retains missing slots");

    for (const bool isProgram : { false, true })
        for (const bool big : { false, true }) {
            const auto bytes = chunkContainer(isProgram, big);
            check(loadPresetFile(bytes.data(), bytes.size(), current, result, error), "Chunk container import");
            check(result.currentProgram == 1 && bits(result.bank[0].values[0]) == 0x80000000u,
                  "Chunk container payload retains own byte order and selected program");
            if (isProgram)
                check(result.bank[1].name[0] == 'P' && result.bank[1].name[27] == 'P' && result.bank[1].name[28] == 0,
                      "FPCh outer name applies after chunk selection");
            else
                check(static_cast<unsigned char>(result.bank[1].name[0]) == 160, "FBCh preserves raw chunk names");
            check(sameProgram(result.bank[100], current.bank[100]), "Chunk container retains missing slots");
        }
    for (const auto& valid : { fxProgram(), fxBank(), chunkContainer(false, false), chunkContainer(true, true) }) {
        auto zeroSizeHeader = valid;
        putWord(zeroSizeHeader, 4, 0);
        for (std::size_t prefix = 0; prefix <= 164; ++prefix)
            fileMustFail(Bytes(zeroSizeHeader.begin(), zeroSizeHeader.begin() + static_cast<std::ptrdiff_t>(prefix)), current);
        for (const auto offset : { 16u, 12u, 4u }) {
            auto invalid = valid;
            putWord(invalid, offset, offset == 12 ? 2 : 0x12345678u);
            fileMustFail(invalid, current);
        }
        auto truncated = valid;
        truncated.pop_back();
        fileMustFail(truncated, current);
        auto trailing = valid;
        trailing.push_back(0);
        fileMustFail(trailing, current);
    }
    auto wrongNestedId = bank;
    putWord(wrongNestedId, 156 + (56 + 38 * 4) + 16, 0x12345678u);
    fileMustFail(wrongNestedId, current);
    auto wrongParameterCount = fxProgram();
    putWord(wrongParameterCount, 24, 37);
    fileMustFail(wrongParameterCount, current);
    auto wrongPayloadSize = chunkContainer(true, false);
    putWord(wrongPayloadSize, 56, 0xffffffffu);
    fileMustFail(wrongPayloadSize, current);
    auto badFloat = fxProgram();
    putWord(badFloat, 56 + 37 * 4, 0x7fc00000u);
    fileMustFail(badFloat, current);
    auto badCount = fxBank();
    putWord(badCount, 24, 0xffffffffu);
    fileMustFail(badCount, current);
    auto unknown = program;
    putMagic(unknown, 8, "BAD!");
    fileMustFail(unknown, current);
    const auto raw = rawFixture(false, false);
    auto aliased = current;
    check(loadPresetFile(raw.data(), raw.size(), aliased, aliased, error), "Aliased current/output import");
    check(sameProgram(aliased.bank[100], current.bank[100]), "Aliased import retains current missing slots");
}

} // namespace

int main(int argc, char** argv) {
    try {
        testFactory(argc > 1 ? argv[1] : "analysis/recovered/factory-bank.rawchunk");
        testRaw();
        testContainers();
        std::cout << "Preset checks passed: factory binary match, LE/BE raw chunks, float-bit preservation, "
                     "partial-state semantics, four FXP/FXB variants, and transactional malformed-input rejection.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Preset check failed: " << error.what() << '\n';
        return 1;
    }
}
