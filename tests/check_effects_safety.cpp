#include "SynthEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

// Count C++ heap activity only inside explicitly marked audio-thread calls.
// prepare(), fixture buffers, diagnostics, and file I/O are outside the guard.
// This catches operator new/delete (including aligned allocation), not direct
// malloc/free hidden inside a system library; source review covers that limit.
namespace allocation_check {
std::atomic<bool> active{false};
std::atomic<std::size_t> allocations{0}, deallocations{0};
void allocated() noexcept {
    if (active.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
}
void freed(void* p) noexcept {
    if (p && active.load(std::memory_order_relaxed)) deallocations.fetch_add(1, std::memory_order_relaxed);
}
}
void* operator new(std::size_t count) {
    allocation_check::allocated();
    if (void* p = std::malloc(std::max(count, std::size_t{1}))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t count) { return ::operator new(count); }
void operator delete(void* p) noexcept { allocation_check::freed(p); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t count, std::align_val_t alignment) {
    allocation_check::allocated();
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(alignment), std::max(count, std::size_t{1})) == 0) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t count, std::align_val_t alignment) { return ::operator new(count, alignment); }
void operator delete(void* p, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ::operator delete(p); }

namespace {
std::size_t realtimeCalls = 0, renderedChannelSamples = 0;
float maximumPanicDifference = 0.0f;
template<class Function> void realtime(Function&& function) {
    allocation_check::active.store(true, std::memory_order_relaxed);
    function();
    allocation_check::active.store(false, std::memory_order_relaxed);
    ++realtimeCalls;
    if (allocation_check::allocations.load(std::memory_order_relaxed) != 0 ||
        allocation_check::deallocations.load(std::memory_order_relaxed) != 0)
        throw std::runtime_error("Heap allocation or deallocation in audio-thread operation");
}
void finite(const float* left, const float* right, int count) {
    for (int i = 0; i < count; ++i)
        if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
            throw std::runtime_error("Nonfinite full-path output");
    renderedChannelSamples += static_cast<std::size_t>(count) * 2;
}
float selector(int effect) { return effect == 0 ? 0.0f : static_cast<float>(effect) / 10.0f; }
struct Audio {
    std::vector<float> left, right;
    explicit Audio(int frames) : left(static_cast<std::size_t>(frames)), right(static_cast<std::size_t>(frames)) {}
};
void render(plasticz::SynthEngine& engine, Audio& output, int begin, int end, int block = 127) {
    for (int at = begin; at < end;) {
        const int count = std::min(block, end - at);
        realtime([&] { engine.render(output.left.data() + at, output.right.data() + at, count); });
        finite(output.left.data() + at, output.right.data() + at, count);
        at += count;
    }
}
void configure(plasticz::SynthEngine& engine, const plasticz::Parameters& parameters, double rate, bool prepare) {
    if (prepare) engine.prepare(rate, 1024);
    realtime([&] {
        engine.setParameters(parameters);
        engine.reset();
        engine.setParameters(parameters);
        engine.setTempo(120.0);
    });
}
void equal(const Audio& a, const Audio& b, const char* what) {
    const auto bytes = a.left.size() * sizeof(float);
    if (a.left.size() != b.left.size() ||
        std::memcmp(a.left.data(), b.left.data(), bytes) ||
        std::memcmp(a.right.data(), b.right.data(), bytes))
        throw std::runtime_error(what);
}
Audio scenario(plasticz::SynthEngine& engine, const plasticz::Parameters& original, double rate) {
    constexpr int frames = 8192, panicFrame = 7168;
    Audio audio(frames);
    auto current = original;
    int at = 0;
    const auto until = [&](int end) { render(engine, audio, at, end); at = end; };
    realtime([&] { engine.noteOn(24, 100.0f / 127.0f); }); until(37);
    realtime([&] { engine.noteOn(60, .63f); }); until(511);
    realtime([&] { engine.pitchBend(.75f); }); until(1024);
    realtime([&] { engine.modWheel(.5f); engine.setExpression(.7f); }); until(1536);
    realtime([&] { engine.noteOff(24); }); until(2048);
    current[28] = selector((static_cast<int>(original[28] * 10.989999771118164f) + 1) % 11);
    realtime([&] { engine.setParameters(current); }); until(3072);
    current[36] = 1.0f;
    current[37] = 1.0f - current[37];
    realtime([&] { engine.setParameters(current); }); until(4096);
    current[36] = 0.0f;
    current[37] = original[37];
    current[29] = 1.0f - current[29];
    realtime([&] { engine.setParameters(current); engine.setTempo(177.0); }); until(5120);
    // Exercise program reset on an already-running instance and reinitialize
    // controls exactly as the processor's parameter snapshot/reset path does.
    current = original;
    realtime([&] { engine.setParameters(current); engine.reset(); engine.noteOn(48, .8f); }); until(6144);
    realtime([&] { engine.noteOff(48); engine.allNotesOff(false); }); until(panicFrame);
    realtime([&] { engine.allNotesOff(true); });
    for (float history : engine.downsamplerState())
        if (history != 0.0f) throw std::runtime_error("Panic retained 4x filter history for the next note");
    until(frames);

    // Silence contains the original filters' deliberate bias. Compare panic
    // against a clean no-note instance instead of requiring absolute zero.
    plasticz::SynthEngine clean;
    configure(clean, current, rate, true);
    realtime([&] { clean.setTempo(177.0); });
    Audio silence(frames - panicFrame);
    render(clean, silence, 0, frames - panicFrame);
    for (int i = 0; i < frames - panicFrame; ++i) {
        maximumPanicDifference = std::max(maximumPanicDifference,
            std::abs(audio.left[static_cast<std::size_t>(panicFrame + i)] - silence.left[static_cast<std::size_t>(i)]));
        maximumPanicDifference = std::max(maximumPanicDifference,
            std::abs(audio.right[static_cast<std::size_t>(panicFrame + i)] - silence.right[static_cast<std::size_t>(i)]));
    }
    if (maximumPanicDifference > 1.0e-7f)
        throw std::runtime_error("Panic retains signal beyond clean original bias");
    return audio;
}

void extreme(plasticz::SynthEngine& engine, int effect, float endpoint, double rate, double tempo) {
    plasticz::Parameters parameters{};
    parameters.fill(endpoint);
    parameters[28] = selector(effect);
    parameters[35] = 1.0f;
    // Both scalar and original 4x rendering are exercised at control endpoints.
    configure(engine, parameters, rate, false);
    realtime([&] {
        engine.setTempo(tempo);
        engine.noteOn(0, 1.0f);
        engine.noteOn(60, 1.0f);
        engine.noteOn(127, 1.0f);
        engine.pitchBend(endpoint);
        engine.modWheel(endpoint);
    });
    Audio audio(4096);
    render(engine, audio, 0, 2048, 257);
    parameters[36] = 1.0f - endpoint;
    realtime([&] { engine.setParameters(parameters); engine.allNotesOff(false); });
    render(engine, audio, 2048, 3072, 257);
    realtime([&] { engine.allNotesOff(true); });
    render(engine, audio, 3072, 4096, 257);
}
}

int main() {
    try {
        const auto bank = plasticz::factoryBank();
        std::size_t factoryCases = 0, extremeCases = 0;
        for (const double rate : {44100.0, 48000.0, 96000.0}) {
            plasticz::SynthEngine engine;
            for (std::size_t preset = 0; preset < bank.size(); ++preset)
              for (const float oversampling : {0.0f, 1.0f}) {
                try {
                    auto parameters = bank[preset].values;
                    parameters[37] = oversampling;
                    configure(engine, parameters, rate, true);
                    const auto initial = scenario(engine, parameters, rate);
                    configure(engine, parameters, rate, true);
                    const auto reprepare = scenario(engine, parameters, rate);
                    equal(initial, reprepare, "Repeated prepare changes full output");
                    configure(engine, parameters, rate, false);
                    const auto programReset = scenario(engine, parameters, rate);
                    equal(initial, programReset, "Program reset changes full output");
                    ++factoryCases;
                } catch (const std::exception& e) {
                    throw std::runtime_error("Preset " + std::to_string(preset) + " rate " + std::to_string(rate)
                        + " 4x " + std::to_string(oversampling) + ": " + e.what());
                }
            }
            engine.prepare(rate, 1024);
            for (int effect = 0; effect <= 10; ++effect)
                for (const float endpoint : {0.0f, 1.0f})
                    for (const double tempo : {1.0, 20.0, 120.0, 1000.0}) {
                        try {
                            extreme(engine, effect, endpoint, rate, tempo);
                            ++extremeCases;
                        } catch (const std::exception& e) {
                            throw std::runtime_error("Extreme effect " + std::to_string(effect) + " rate " + std::to_string(rate) +
                                " endpoint " + std::to_string(endpoint) + " tempo " + std::to_string(tempo) + ": " + e.what());
                        }
                    }
        }
        std::cout << "Full-path safety: " << factoryCases << " factory/rate/oversampling cases, each exact after repeated prepare and program reset; "
                  << extremeCases << " effect/control/tempo extremes; " << renderedChannelSamples << " finite channel samples; "
                  << realtimeCalls << " guarded audio-thread calls, zero C++ heap allocations/deallocations; max panic/bias difference="
                  << maximumPanicDifference << '\n';
        return 0;
    } catch (const std::exception& e) {
        allocation_check::active.store(false, std::memory_order_relaxed);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
