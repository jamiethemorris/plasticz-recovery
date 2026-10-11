#include "Waveforms.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

static std::uint32_t bits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: check_waveforms cosine-table.f32 waveforms.csv\n";
            return 2;
        }
        plasticz::Waveforms::Table table;
        std::ifstream tableFile(argv[1], std::ios::binary);
        tableFile.read(reinterpret_cast<char*>(table.data()), sizeof(table));
        if (tableFile.gcount() != sizeof(table) || tableFile.peek() != EOF)
            throw std::runtime_error("Expected exactly 16384 little-endian float32 table entries");
        const plasticz::Waveforms waveforms(table);
        const plasticz::Waveforms nativeTableGenerator;
        std::size_t tableMismatches = 0;
        for (std::size_t i = 0; i < table.size(); ++i)
            if (bits(table[i]) != bits(nativeTableGenerator.cosineTable()[i]))
                ++tableMismatches;
        std::ifstream input(argv[2]);
        if (!input) throw std::runtime_error("Cannot read waveform fixtures");
        std::string line;
        std::getline(input, line); // CSV header
        std::size_t count = 0, failures = 0;
        float maxError = 0;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            for (auto& c : line) if (c == ',') c = ' ';
            std::istringstream row(line);
            std::string kind;
            int wave;
            float phase, distortion, expected;
            if (!(row >> kind >> wave >> phase >> distortion >> expected))
                throw std::runtime_error("Malformed fixture row: " + line);
            if (wave < 0 || wave > 7 || (kind != "wave" && kind != "offset"))
                throw std::runtime_error("Unknown fixture kind or waveform");
            auto type = static_cast<plasticz::Waveform>(wave);
            const auto actual = kind == "wave" ? waveforms.evaluate(type, phase, distortion)
                                                : waveforms.dcOffset(type, distortion);
            const float error = std::abs(actual - expected);
            maxError = std::max(maxError, error);
            ++count;
            if (bits(actual) != bits(expected)) {
                if (failures < 12)
                    std::cerr << kind << ' ' << wave << " phase=" << phase
                              << " distortion=" << distortion << " expected=" << expected
                              << " actual=" << actual << " expected_bits=" << bits(expected)
                              << " actual_bits=" << bits(actual) << '\n';
                ++failures;
            }
        }
        if (!count) throw std::runtime_error("No fixtures were read");
        std::cout << "Cosine table: " << table.size() << " entries; native/fixture bit mismatches="
                  << tableMismatches << '\n';
        std::cout << "Compared " << count << " scalar cases; bit mismatches=" << failures
                  << "; max_abs_error=" << maxError << '\n';
        return failures || tableMismatches ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
