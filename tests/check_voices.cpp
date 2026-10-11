#include "SynthEngine.h"

#include <algorithm>
#include <array>
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
std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open " + path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::uint32_t word(const void* source) {
    std::uint32_t value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}
float real(std::uint32_t source) {
    float value;
    std::memcpy(&value, &source, sizeof(value));
    return value;
}
struct Statistics {
    std::size_t states{}, samples{}, stateWordMismatches{}, integerMismatches{}, audioMismatches{};
    float maxStateError{}, maxAudioError{};
    std::array<std::size_t, 23> fields{};
};
constexpr const char* fieldNames[] = {
    "note", "velocity", "dcwLevel", "dcwProgress", "dcwAtRelease", "dcwStage",
    "ampLevel", "ampProgress", "ampAtRelease", "ampStage/held", "smoothAmp",
    "ampTarget", "unused30", "unused34", "dcwTarget", "smoothDcw", "phase1",
    "phase2", "phaseStep", "unused4c", "targetNote", "currentNote", "glideStep"
};
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: check_voices path/to/voice-oracle-directory\n";
            return 2;
        }
        const std::string directory = argv[1];
        const auto expectedStates = readFile(directory + "/voices.bin");
        const auto expectedAudio = readFile(directory + "/audio-mono.f32");
        std::ifstream commands(directory + "/commands.txt");
        std::string line;
        std::getline(commands, line);
        if (line != "PLASTICZ_VOICE_ORACLE 1") throw std::runtime_error("Unsupported commands header");
        std::unique_ptr<plasticz::SynthEngine> engine;
        plasticz::Parameters parameters{};
        std::string caseName;
        double sampleRate = 0;
        std::size_t step = 0, stateMessages = 0, audioMessages = 0, cases = 0;
        Statistics totals, perCase;
        while (std::getline(commands, line)) {
            std::istringstream row(line);
            std::string action;
            row >> action;
            if (action == "case") {
                if (!(row >> caseName >> sampleRate)) throw std::runtime_error("Malformed case");
                engine = std::make_unique<plasticz::SynthEngine>();
                step = 0;
                perCase = {};
                ++cases;
                continue;
            }
            if (action == "parameters") {
                for (auto& value : parameters)
                    if (!(row >> value)) throw std::runtime_error("Malformed parameter list");
                continue;
            }
            if (action == "end") {
                std::cout << caseName << ": state_word_mismatches=" << perCase.stateWordMismatches
                          << " integer_mismatches=" << perCase.integerMismatches
                          << " audio_bit_mismatches=" << perCase.audioMismatches
                          << " max_state_error=" << perCase.maxStateError
                          << " max_audio_error=" << perCase.maxAudioError << '\n';
                continue;
            }
            std::size_t expectedOffset;
            if (!engine || !(row >> expectedOffset)) throw std::runtime_error("Malformed action: " + line);
            if (action == "initialize") {
                engine->setParameters(parameters);
                engine->prepare(sampleRate, 512);
            } else if (action == "control") {
                engine->controlTick();
            } else if (action == "note_on") {
                int note, channel;
                float velocity;
                if (!(row >> note >> velocity >> channel) || channel != 0) throw std::runtime_error("Malformed note_on");
                engine->noteOn(note, velocity);
            } else if (action == "note_off") {
                int note, channel;
                if (!(row >> note >> channel) || channel != 0) throw std::runtime_error("Malformed note_off");
                engine->noteOff(note);
            } else if (action == "render") {
                std::size_t frames, offset, audioOffset;
                if (!(row >> frames >> offset >> audioOffset) || frames > 32768 || offset > 32768)
                    throw std::runtime_error("Malformed render");
                if (audioOffset + frames * 4 > expectedAudio.size()) throw std::runtime_error("Audio fixture out of bounds");
                std::vector<float> actual(frames + offset);
                engine->renderDry(actual.data() + offset, static_cast<int>(frames));
                for (std::size_t i = 0; i < frames; ++i) {
                    const auto wanted = word(expectedAudio.data() + audioOffset + i * 4);
                    const auto got = word(actual.data() + offset + i);
                    if (wanted != got) {
                        ++totals.audioMismatches;
                        ++perCase.audioMismatches;
                        const float error = std::abs(real(wanted) - real(got));
                        totals.maxAudioError = std::max(totals.maxAudioError, error);
                        perCase.maxAudioError = std::max(perCase.maxAudioError, error);
                        if (audioMessages++ < 12)
                            std::cerr << caseName << " step=" << step << " sample=" << i
                                      << " wanted=" << std::setprecision(9) << real(wanted) << " got=" << real(got)
                                      << " expected_bits=" << std::hex << wanted << " actual_bits=" << got << std::dec << '\n';
                    }
                    ++totals.samples;
                }
            } else throw std::runtime_error("Unknown action: " + action);

            constexpr auto stateSize = sizeof(plasticz::VoiceState) * 16;
            if (expectedOffset + stateSize > expectedStates.size()) throw std::runtime_error("State fixture out of bounds");
            const auto* actual = reinterpret_cast<const std::uint8_t*>(engine->voiceStates().data());
            for (std::size_t i = 0; i < stateSize / 4; ++i) {
                const auto wanted = word(expectedStates.data() + expectedOffset + i * 4);
                const auto got = word(actual + i * 4);
                if (wanted == got) continue;
                const auto field = i % 23;
                const bool integral = field == 0 || field == 5 || field == 9;
                ++totals.stateWordMismatches;
                ++perCase.stateWordMismatches;
                ++totals.fields[field];
                if (integral) { ++totals.integerMismatches; ++perCase.integerMismatches; }
                else {
                    const float error = std::abs(real(wanted) - real(got));
                    totals.maxStateError = std::max(totals.maxStateError, error);
                    perCase.maxStateError = std::max(perCase.maxStateError, error);
                }
                if (stateMessages++ < 24)
                    std::cerr << caseName << " step=" << step << " action=" << action
                              << " voice=" << i / 23 << " field=" << fieldNames[field]
                              << " wanted=" << std::setprecision(9) << real(wanted) << " got=" << real(got)
                              << " expected_bits=" << std::hex << wanted << " actual_bits=" << got << std::dec << '\n';
            }
            ++totals.states;
            ++step;
        }
        if (!cases || !totals.states || !totals.samples) throw std::runtime_error("No comparison fixtures");
        std::cout << "Compared " << cases << " cases, " << totals.states << " full 16-voice snapshots, "
                  << totals.samples << " mono samples; state_word_mismatches=" << totals.stateWordMismatches
                  << " integer_mismatches=" << totals.integerMismatches
                  << " audio_bit_mismatches=" << totals.audioMismatches
                  << " max_state_error=" << totals.maxStateError << " max_audio_error=" << totals.maxAudioError << '\n';
        for (std::size_t i = 0; i < totals.fields.size(); ++i)
            if (totals.fields[i]) std::cout << "field " << fieldNames[i] << " mismatches=" << totals.fields[i] << '\n';
        return totals.stateWordMismatches || totals.audioMismatches ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
