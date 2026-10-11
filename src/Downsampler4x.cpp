#include "Downsampler4x.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace plasticz {
namespace {
float fromBits(std::uint32_t word) noexcept {
    float result;
    std::memcpy(&result, &word, sizeof(result));
    return result;
}
}

float Downsampler4x::step(float input) noexcept {
    // Coefficients are the literal words written by constructor 0x281a6.
    // Keep the original additions separate: ARM FMA changes stored histories.
    const auto& h = history_;
    float a = input + h[0] * fromBits(0x3fb365ce);
    a = a + h[1];
    a = a + h[2] * fromBits(0x3fc1ddc6);
    a = a + h[3] * fromBits(0xbf18e482);
    float b = h[2] * fromBits(0xbe81e17d) + a;
    b = b + h[3];
    b = b + h[4] * fromBits(0x3fbb19a7);
    b = b + h[5] * fromBits(0xbf352ddd);
    float c = h[4] * fromBits(0xbf5deb2e) + b;
    c = c + h[5];
    c = c + h[6] * fromBits(0x3fb4b682);
    c = c + h[7] * fromBits(0xbf57691b);
    float d = h[6] * fromBits(0xbf86b870) + c;
    d = d + h[7];
    d = d * fromBits(0x3a3e4f85);
    d = d + h[8] * fromBits(0x3fb44f69);
    d = d + h[9] * fromBits(0xbf7374ab);
    history_ = {input, h[0], a, h[2], b, h[4], c, h[6], d, h[8]};
    return d;
}

void Downsampler4x::process(const float* input, float* output, int outputFrames) noexcept {
    for (int i = 0; i < outputFrames; ++i) {
        step(input[0]);
        step(input[1]);
        step(input[2]);
        output[i] = step(input[3]);
        input += 4;
    }
}

void Downsampler4x::finishBlock() noexcept {
    // The two input-history slots are intentionally excluded.
    for (std::size_t i = 2; i < history_.size(); ++i)
        if (std::fabs(history_[i]) < fromBits(0x2b8cbccc)) history_[i] = 0.0f;
}

} // namespace plasticz
