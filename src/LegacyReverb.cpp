#include "LegacyReverb.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// The original processing uses separate scalar SSE operations. This source
// must be compiled with -ffp-contract=off -fno-fast-math.
namespace plasticz {
namespace {
constexpr float bias = 1.000000045813705e-18f; // original 0x3f68c, 0x219392ef
constexpr float millisecondsToSeconds = 0.0010000000474974513f;
constexpr float predelayScale = 200.99000549316406f;
constexpr float stereoSpread = 0.5220000147819519f;
constexpr std::array<float, 8> combMilliseconds{
    25.309999465942383f, 26.940000534057617f, 28.959999084472656f,
    30.75f, 32.244998931884766f, 33.810001373291016f,
    35.310001373291016f, 36.66699981689453f};
constexpr std::array<float, 4> allpassMilliseconds{
    12.609999656677246f, 10.0f, 7.739999771118164f, 5.110000133514404f};
void decrement(int& position, int length) noexcept {
    --position;
    if (position < 0) position += length;
}
}

void LegacyReverb::prepare(float sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0f || sampleRate > 384000.0f)
        throw std::invalid_argument("LegacyReverb sample rate must be 8000..384000 Hz");
    if (ready_ && sampleRate == sampleRate_) return;
    sampleRate_ = sampleRate;
    ready_ = false;
    for (unsigned i = 0; i < combs_.size(); ++i) {
        auto& delay = combs_[i];
        float time = combMilliseconds[i % 8];
        if (i > 7) time += stereoSpread;
        delay.length = static_cast<int>((time * sampleRate_) * millisecondsToSeconds);
        delay.samples.assign(static_cast<std::size_t>(delay.length), 0.0f);
        delay.position = 0;
        // 0x26604 does not overwrite comb.history, even on rate changes.
    }
    for (unsigned i = 0; i < allpasses_.size(); ++i) {
        auto& delay = allpasses_[i];
        float time = allpassMilliseconds[i % 4];
        if (i > 3) time += stereoSpread;
        delay.length = static_cast<int>((time * sampleRate_) * millisecondsToSeconds);
        delay.samples.assign(static_cast<std::size_t>(delay.length), 0.0f);
        delay.position = 0;
    }
    const auto capacity = static_cast<std::size_t>(0.2f * sampleRate_);
    for (auto& delay : predelays_) {
        // Allocation happens while the object's ready flag is false, so the
        // original skips its attempted predelay-parameter application here.
        delay.samples.assign(capacity, 0.0f);
        // Safely bound the original stale-length rate-change edge case. The
        // plug-in never configures predelay; this only affects standalone use.
        if (static_cast<std::size_t>(delay.length) > capacity) {
            delay.length = static_cast<int>(capacity);
            delay.position = 0;
        }
    }
    ready_ = true;
}

void LegacyReverb::reset() noexcept {
    if (!ready_) return;
    for (auto& delay : combs_) std::fill(delay.samples.begin(), delay.samples.end(), 0.0f);
    for (auto& delay : allpasses_) std::fill(delay.samples.begin(), delay.samples.end(), 0.0f);
    for (auto& delay : predelays_)
        std::fill_n(delay.samples.begin(), delay.length, 0.0f);
}

void LegacyReverb::resetFresh() noexcept {
    for (auto& delay : combs_) {
        std::fill(delay.samples.begin(), delay.samples.end(), 0.0f);
        delay.position = 0;
        delay.history = 0.0f;
    }
    for (auto& delay : allpasses_) {
        std::fill(delay.samples.begin(), delay.samples.end(), 0.0f);
        delay.position = 0;
    }
    for (auto& delay : predelays_) {
        std::fill(delay.samples.begin(), delay.samples.end(), 0.0f);
        delay.position = 0;
    }
}

void LegacyReverb::setPredelay(unsigned channel, int milliseconds) noexcept {
    if (!ready_) return;
    auto& delay = predelays_[channel];
    const int next = static_cast<int>((static_cast<float>(milliseconds) * sampleRate_) * millisecondsToSeconds);
    if (next == delay.length) return;
    // The original assumes normalized controls; retain memory safety for direct
    // callers while preserving every normalized control value.
    delay.length = std::clamp(next, 0, static_cast<int>(delay.samples.size()));
    delay.position = 0;
}

void LegacyReverb::configure(unsigned parameter, float value) noexcept {
    if (parameter >= parameters_.size() || !std::isfinite(value)) return;
    parameters_[parameter] = value;
    switch (parameter) {
    case 2: {
        const float angle = value * 1.5707963705062866f;
        parameters_[0] = std::cos(angle);
        parameters_[1] = std::sin(angle);
        break;
    }
    case 4: setPredelay(0, static_cast<int>(value * predelayScale)); break;
    case 5: setPredelay(1, static_cast<int>(value * predelayScale)); break;
    case 10:
        parameters_[4] = parameters_[5] = value;
        setPredelay(0, static_cast<int>(value * predelayScale));
        setPredelay(1, static_cast<int>(value * predelayScale));
        break;
    case 11: parameters_[6] = parameters_[7] = value; break;
    case 12: parameters_[8] = parameters_[9] = value; break;
    default: break;
    }
}

float LegacyReverb::predelay(unsigned channel, float input) noexcept {
    auto& delay = predelays_[channel];
    if (delay.length == 0) return input;
    const float result = delay.samples[static_cast<std::size_t>(delay.position)];
    delay.samples[static_cast<std::size_t>(delay.position)] = input;
    decrement(delay.position, delay.length);
    return result;
}

float LegacyReverb::network(unsigned channel, float input, float room, float damp) noexcept {
    const float excitation = input * 0.014999999664723873f + bias;
    float sum = 0.0f;
    for (unsigned i = channel * 8; i < channel * 8 + 8; ++i) {
        auto& delay = combs_[i];
        auto& slot = delay.samples[static_cast<std::size_t>(delay.position)];
        const float value = slot;
        delay.history = ((delay.history - value) * damp + value) + bias;
        slot = delay.history * room + excitation;
        decrement(delay.position, delay.length);
        sum += value;
    }
    for (unsigned i = channel * 4; i < channel * 4 + 4; ++i) {
        auto& delay = allpasses_[i];
        auto& slot = delay.samples[static_cast<std::size_t>(delay.position)];
        const float value = slot;
        slot = value * 0.5f + sum;
        sum = value - sum;
        decrement(delay.position, delay.length);
    }
    return sum;
}

void LegacyReverb::process(const float* const input[2], float* const output[2], int frames) noexcept {
    if (!ready_ || frames <= 0) return;
    const float roomL = parameters_[6] * 0.2800000011920929f + 0.699999988079071f;
    const float roomR = parameters_[7] * 0.2800000011920929f + 0.699999988079071f;
    const float dampL = parameters_[8] * 0.4000000059604645f;
    const float dampR = parameters_[9] * 0.4000000059604645f;
    float dry = parameters_[0];
    if (input[0] == output[0]) dry -= 1.0f;
    // Original DSP establishes x87 round-to-zero before FISTP at 0x25f88.
    const int mode = static_cast<int>(parameters_[3]);
    if (mode == 0) {
        const float wet = ((0.5f * parameters_[13] + 0.5f) * parameters_[1]) * 3.0f;
        const float cross = ((1.0f - parameters_[13]) * parameters_[1]) * 1.5f;
        for (int i = 0; i < frames; ++i) {
            const float mono = predelay(0, (input[0][i] + input[1][i]) * 0.5f);
            const float left = network(0, mono, roomL, dampL);
            // Preserve the original asymmetric width path: only the right
            // output receives the opposite channel's reverb contribution.
            output[0][i] = (wet * left + dry * input[0][i]) + output[0][i];
            const float right = network(1, mono, roomR, dampR);
            output[1][i] = ((right * wet + left * cross) + dry * input[1][i]) + output[1][i];
        }
    } else if (mode == 1) {
        const float wet = 3.0f * parameters_[1];
        for (unsigned channel = 0; channel < 2; ++channel)
            for (int i = 0; i < frames; ++i) {
                const float value = network(channel, predelay(channel, input[channel][i]),
                                            channel == 0 ? roomL : roomR, channel == 0 ? dampL : dampR);
                output[channel][i] = (value * wet + output[channel][i]) + dry * input[channel][i];
            }
    }
}

void LegacyReverb::process(float* left, float* right, int frames) noexcept {
    const float* input[]{left, right};
    float* output[]{left, right};
    process(input, output, frames);
}

} // namespace plasticz
