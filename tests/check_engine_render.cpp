#include "SynthEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
constexpr int frames = 13000;
constexpr std::array<int, 12> events{0, 37, 113, 700, 900, 1350, 1600, 2000, 2500, 5000, 6000, 12000};
std::vector<float> render(const plasticz::Parameters& parameters, double rate, int block) {
    plasticz::SynthEngine engine;
    engine.setParameters(parameters);
    engine.prepare(rate, block);
    engine.setFullProcessingEnabled(false);
    std::vector<float> left(frames), right(frames);
    int at = 0;
    for (std::size_t event = 0; event < events.size(); ++event) {
        switch (event) {
            case 0: engine.noteOn(60, 0.75f); break;
            case 1: engine.noteOn(64, 0.5f); break;
            case 2: engine.noteOn(67, 1.0f); break;
            case 3: engine.noteOn(79, 0.8f); break;
            case 4: engine.pitchBend(0.8f); break;
            case 5: engine.modWheel(1.0f); break;
            case 6: engine.setExpression(0.5f); break;
            case 7: engine.noteOff(64); break;
            case 8: engine.noteOff(79); break;
            case 9: engine.allNotesOff(false); break;
            case 10:
                // Exercise bounded mono note storage and poly stealing.
                for (int i = 0; i < 256; ++i) engine.noteOn(24 + i % 96, 0.7f);
                break;
            case 11: engine.allNotesOff(true); break;
        }
        const int end = event + 1 < events.size() ? events[event + 1] : frames;
        while (at < end) {
            const int count = std::min(block, end - at);
            engine.render(left.data() + at, right.data() + at, count);
            at += count;
        }
    }
    if (std::memcmp(left.data(), right.data(), frames * sizeof(float)))
        throw std::runtime_error("Dry stereo channels differ");
    for (int i = 0; i < frames; ++i) {
        if (!std::isfinite(left[i])) throw std::runtime_error("Non-finite audio");
        if (i >= 12000 && left[i] != 0) throw std::runtime_error("Panic did not silence output");
    }
    return left;
}
}
int main() {
    try {
        const auto bank = plasticz::factoryBank();
        std::size_t cases = 0;
        float maximumBlockDifference = 0;
        for (const double rate : {44100.0, 48000.0, 96000.0})
            for (const auto& program : bank) {
                const auto whole = render(program.values, rate, 8192);
                const auto split = render(program.values, rate, 127);
                for (int i = 0; i < frames; ++i)
                    maximumBlockDifference = std::max(maximumBlockDifference, std::abs(whole[i] - split[i]));
                ++cases;
            }
        auto invalid = bank[0].values;
        invalid[0] = std::numeric_limits<float>::quiet_NaN();
        invalid[8] = std::numeric_limits<float>::infinity();
        render(invalid, std::numeric_limits<double>::quiet_NaN(), 257);
        std::cout << "Checked " << cases << " preset/rate cases: finite audio, stereo equality, note overflow, panic; invalid-input sanitization passed; max block-size difference=" << maximumBlockDifference << '\n';
        // sub_f8d0 checks the 1/32768 voice-tail cutoff at call boundaries.
        // Splitting calls can therefore omit a few additional near-silent
        // samples. Keep the exact dry primitive and bound this known difference.
        if (maximumBlockDifference > 0.0001f)
            throw std::runtime_error("Audio changes materially across host block sizes");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
