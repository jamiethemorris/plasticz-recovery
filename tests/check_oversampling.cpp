#include "Downsampler4x.h"
#include "SynthEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<char> file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::uint32_t word(const void* at) {
    std::uint32_t result;
    std::memcpy(&result, at, sizeof(result));
    return result;
}
void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: check_oversampling fixture-directory");
        const std::string directory = argv[1];
        const auto inputs = file(directory + "/inputs.f32");
        const auto audio = file(directory + "/audio.f32");
        const auto states = file(directory + "/states.bin");
        std::ifstream commands(directory + "/commands.txt");
        std::string line, name;
        std::getline(commands, line);
        require(line == "PLASTICZ_OVERSAMPLING_ORACLE 1", "Wrong fixture schema");
        std::unique_ptr<plasticz::SynthEngine> engine;
        plasticz::Downsampler4x decimator;
        plasticz::Parameters parameters{};
        float rate = 0;
        bool full = false;
        std::size_t cases = 0, sampleWords = 0, stateWords = 0, audioBad = 0, stateBad = 0;
        std::size_t caseAudioBad = 0, caseStateBad = 0, caseSamples = 0;
        double maximum = 0, squared = 0;

        auto compareState = [&](const void* native, std::size_t bytes, std::size_t offset) {
            require(offset + bytes <= states.size(), "State fixture out of range");
            const auto* actual = static_cast<const char*>(native);
            for (std::size_t i = 0; i < bytes; i += 4) {
                const auto a = word(actual + i), e = word(states.data() + offset + i);
                if (a != e) {
                    if (stateBad < 8) std::cerr << name << " state word=" << i / 4 << " fixture_byte=" << offset + i
                        << " actual=" << std::hex << a << " expected=" << e << std::dec << '\n';
                    ++stateBad;
                    ++caseStateBad;
                }
                ++stateWords;
            }
        };
        auto compareAudio = [&](const std::vector<float>& actual, std::size_t offset) {
            require(offset + actual.size() * 4 <= audio.size(), "Audio fixture out of range");
            for (std::size_t i = 0; i < actual.size(); ++i) {
                float expected;
                std::memcpy(&expected, audio.data() + offset + i * 4, 4);
                require(std::isfinite(actual[i]) && std::isfinite(expected), "Nonfinite audio");
                if (word(&actual[i]) != word(&expected)) {
                    if (audioBad < 8) std::cerr << name << " audio fixture_byte=" << offset + i * 4
                        << " actual=" << std::hex << word(&actual[i]) << " expected=" << word(&expected) << std::dec << '\n';
                    ++audioBad;
                    ++caseAudioBad;
                }
                const double error = double(actual[i]) - expected;
                maximum = std::max(maximum, std::fabs(error));
                squared += error * error;
            }
            sampleWords += actual.size();
            caseSamples += actual.size();
        };

        while (std::getline(commands, line)) {
            std::istringstream row(line);
            std::string action;
            row >> action;
            if (action == "case" || action == "downsampler") {
                require(bool(row >> name), "Missing case name");
                ++cases;
                caseAudioBad = caseStateBad = caseSamples = 0;
                if (action == "case") {
                    int mode;
                    require(bool(row >> rate >> mode), "Missing case settings");
                    full = mode != 0;
                    engine = std::make_unique<plasticz::SynthEngine>();
                } else decimator.reset();
            } else if (action == "parameters") {
                for (auto& value : parameters) require(bool(row >> value), "Missing parameter");
            } else if (action == "prepare") {
                engine->setParameters(parameters);
                engine->prepare(rate, 512);
                engine->setFullProcessingEnabled(full);
            } else if (action == "parameter") {
                std::size_t index;
                float value;
                require(bool(row >> index >> value) && index < parameters.size(), "Bad parameter action");
                parameters[index] = value;
                engine->setParameters(parameters);
            } else if (action == "control") {
                engine->controlTick();
            } else if (action == "note_on") {
                int note;
                float velocity;
                require(bool(row >> note >> velocity), "Bad note-on");
                engine->noteOn(note, velocity);
            } else if (action == "note_off") {
                int note;
                require(bool(row >> note), "Bad note-off");
                engine->noteOff(note);
            } else if (action == "filter") {
                std::size_t count, inputOffset, audioOffset, stateOffset;
                require(bool(row >> count >> inputOffset >> audioOffset >> stateOffset), "Bad filter action");
                require(inputOffset + count * 16 <= inputs.size(), "Input fixture out of range");
                std::vector<float> input(count * 4), output(count);
                if (count) std::memcpy(input.data(), inputs.data() + inputOffset, count * 16);
                decimator.process(input.data(), output.data(), static_cast<int>(count));
                if (count) decimator.finishBlock();
                compareAudio(output, audioOffset);
                compareState(decimator.history().data(), 40, stateOffset);
            } else if (action == "dry" || action == "host") {
                std::size_t count, audioOffset, stateOffset;
                require(bool(row >> count >> audioOffset >> stateOffset), "Bad render action");
                std::vector<float> left(count), right(count), output;
                if (action == "dry") {
                    engine->renderDry(left.data(), static_cast<int>(count));
                    output = left;
                } else {
                    engine->render(left.data(), right.data(), static_cast<int>(count));
                    output.resize(count * 2);
                    for (std::size_t i = 0; i < count; ++i) {
                        output[i * 2] = left[i];
                        output[i * 2 + 1] = right[i];
                    }
                }
                compareAudio(output, audioOffset);
                compareState(engine->voiceStates().data(), sizeof(plasticz::VoiceState) * 16, stateOffset);
                compareState(engine->downsamplerState().data(), 40, stateOffset + sizeof(plasticz::VoiceState) * 16);
            } else if (action == "end") {
                std::cout << name << ": samples=" << caseSamples << " audio_mismatches=" << caseAudioBad
                          << " state_mismatches=" << caseStateBad << '\n';
            } else throw std::runtime_error("Unknown action: " + action);
        }
        require(sampleWords * 4 == audio.size() && stateWords * 4 == states.size() && cases == 19,
                "Incomplete comparison");
        std::cout << "TOTAL cases=" << cases << " sample_words=" << sampleWords << " state_words=" << stateWords
                  << " audio_mismatches=" << audioBad << " state_mismatches=" << stateBad
                  << " max_error=" << std::setprecision(12) << maximum << " rms_error=" << std::sqrt(squared / sampleWords) << '\n';
        return audioBad || stateBad ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
