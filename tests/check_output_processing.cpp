#include "OutputProcessing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open " + path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::uint32_t bits(float sample) {
    std::uint32_t result;
    std::memcpy(&result, &sample, sizeof(result));
    return result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: check_output_processing fixture-directory");
        const std::string directory = argv[1];
        const auto inputs = readFile(directory + "/inputs.f32");
        const auto expected = readFile(directory + "/expected.f32");
        std::ifstream commands(directory + "/commands.txt");
        std::string line, name;
        std::getline(commands, line);
        if (line != "PLASTICZ_OUTPUT_ORACLE 1") throw std::runtime_error("Unsupported fixture schema");
        plasticz::OutputProcessing processor;
        std::size_t samples = 0, mismatches = 0, cases = 0, caseMismatches = 0, caseSamples = 0;
        double maximum = 0, squared = 0;
        while (std::getline(commands, line)) {
            std::istringstream row(line);
            std::string action;
            row >> action;
            if (action == "case") {
                float rate;
                if (!(row >> name >> rate)) throw std::runtime_error("Malformed case");
                processor.prepare(rate);
                ++cases;
                caseMismatches = caseSamples = 0;
            } else if (action == "reset") {
                processor.reset();
            } else if (action == "end") {
                std::cout << name << ": samples=" << caseSamples << " bit_mismatches=" << caseMismatches << '\n';
            } else if (action == "input" || action == "output") {
                std::size_t count, inputOffset, expectedOffset;
                if (!(row >> count >> inputOffset >> expectedOffset)) throw std::runtime_error("Malformed process action");
                const auto words = count * (action == "input" ? 1 : 2);
                if (inputOffset + words * 4 > inputs.size() || expectedOffset + words * 4 > expected.size())
                    throw std::runtime_error("Fixture read exceeds file");
                std::vector<float> audio(words), reference(words);
                if (words) {
                    std::memcpy(audio.data(), inputs.data() + inputOffset, words * 4);
                    std::memcpy(reference.data(), expected.data() + expectedOffset, words * 4);
                }
                if (action == "input") processor.processInput(audio.data(), static_cast<int>(count));
                else processor.processOutput(audio.data(), count ? audio.data() + count : nullptr, static_cast<int>(count));
                for (std::size_t i = 0; i < words; ++i) {
                    const auto actualBits = bits(audio[i]), expectedBits = bits(reference[i]);
                    if (actualBits != expectedBits) {
                        if (mismatches < 8) std::cerr << name << ' ' << action << " file_byte=" << expectedOffset + 4*i
                            << " expected_bits=" << std::hex << expectedBits << " actual_bits=" << actualBits << std::dec << '\n';
                        ++mismatches;
                        ++caseMismatches;
                    }
                    const double error = static_cast<double>(audio[i]) - reference[i];
                    if (!std::isfinite(error)) throw std::runtime_error("Non-finite native output");
                    maximum = std::max(maximum, std::fabs(error));
                    squared += error * error;
                }
                samples += words;
                caseSamples += words;
            } else throw std::runtime_error("Unknown fixture action: " + action);
        }
        if (samples * 4 != expected.size() || cases != 6) throw std::runtime_error("Incomplete fixture comparison");
        std::cout << "TOTAL cases=" << cases << " sample_words=" << samples << " bit_mismatches=" << mismatches
                  << " max_error=" << std::setprecision(12) << maximum << " rms_error=" << std::sqrt(squared / samples) << '\n';
        return mismatches ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
