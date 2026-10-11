#include "LegacyReverb.h"

// A narrow C ABI for the instruction-oracle differential runner. This contains
// no DSP implementation and is never linked into the plug-in.
extern "C" {
void* legacy_reverb_create() { return new plasticz::LegacyReverb; }
void legacy_reverb_delete(void* object) { delete static_cast<plasticz::LegacyReverb*>(object); }
void legacy_reverb_prepare(void* object, float rate) {
    static_cast<plasticz::LegacyReverb*>(object)->prepare(rate);
}
void legacy_reverb_reset(void* object) {
    static_cast<plasticz::LegacyReverb*>(object)->reset();
}
void legacy_reverb_reset_fresh(void* object) {
    static_cast<plasticz::LegacyReverb*>(object)->resetFresh();
}
void legacy_reverb_configure(void* object, unsigned parameter, float value) {
    static_cast<plasticz::LegacyReverb*>(object)->configure(parameter, value);
}
void legacy_reverb_process(void* object, const float* left, const float* right,
                           float* outLeft, float* outRight, int frames) {
    const float* input[]{left, right};
    float* output[]{outLeft, outRight};
    static_cast<plasticz::LegacyReverb*>(object)->process(input, output, frames);
}
}
