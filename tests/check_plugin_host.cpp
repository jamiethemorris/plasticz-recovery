#include <juce_audio_utils/juce_audio_utils.h>
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <sstream>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t word(const void* data) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint32_t floatBits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}

std::vector<std::uint32_t> referenceSamples(const juce::File& file, int channels) {
    juce::MemoryBlock fileData;
    require(file.loadFileAsData(fileData), "Cannot load preset reference WAV");
    const auto* bytes = static_cast<const std::uint8_t*>(fileData.getData());
    const auto size = fileData.getSize();
    require(size >= 44 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WAVE", 4) == 0,
            "Expected RIFF WAV reference");
    require(word(bytes + 4) + std::uint64_t{8} == size, "Reference WAV size mismatch");
    bool correctFormat = false;
    for (size_t at = 12; at + 8 <= size;) {
        const auto length = static_cast<size_t>(word(bytes + at + 4));
        require(length <= size - at - 8, "Invalid WAV chunk");
        const auto* data = bytes + at + 8;
        if (std::memcmp(bytes + at, "fmt ", 4) == 0) {
            require(length >= 16 && word(data) == (3u | (static_cast<std::uint32_t>(channels) << 16))
                    && word(data + 4) == 44100 && word(data + 8) == static_cast<std::uint32_t>(44100 * channels * 4)
                    && word(data + 12) == (0x00200000u | static_cast<std::uint32_t>(channels * 4)),
                    "Expected 44100Hz IEEE float32 reference with the selected channel count");
            correctFormat = true;
        }
        if (std::memcmp(bytes + at, "data", 4) == 0) {
            require(correctFormat && length == static_cast<size_t>(264600 * channels * 4), "Expected six-second reference");
            std::vector<std::uint32_t> result(length / 4);
            for (size_t i = 0; i < result.size(); ++i) result[i] = word(data + i * 4);
            return result;
        }
        at += 8 + length + (length & 1u);
    }
    throw std::runtime_error("Reference has no audio data");
}

void processWithHostFpMode(juce::AudioPluginInstance& plugin, juce::AudioBuffer<float>& block,
                           juce::MidiBuffer& midi, bool flushSubnormals) {
    juce::ScopedNoDenormals restoreTestFpState;
    juce::FloatVectorOperations::disableDenormalisedNumberSupport(flushSubnormals);
    plugin.processBlock(block, midi);
    require(juce::FloatVectorOperations::areDenormalsDisabled() == flushSubnormals,
            "Plugin did not restore incoming host floating-point mode");
}

std::uint64_t testPresetSuite(juce::AudioPluginInstance& plugin, const juce::String& format,
                              const juce::File& references, std::ofstream& csv, bool flushSubnormals, bool full) {
    const int referenceChannels = full ? 2 : 1;
    std::uint64_t totalMismatches = 0;
    double totalSquareError = 0, totalMaximum = 0;
    for (int preset = 0; preset < 128; ++preset) {
        const auto name = "preset-" + juce::String(preset).paddedLeft('0', 3) + ".wav";
        const auto expected = referenceSamples(references.getChildFile(name), referenceChannels);
        plugin.setCurrentProgram(preset);
        plugin.prepareToPlay(44100.0, 512);
        juce::AudioBuffer<float> block(2, 512);
        juce::MidiBuffer midi;
        block.clear();
        processWithHostFpMode(plugin, block, midi, flushSubnormals); // One discarded silent preroll block.
        std::uint64_t mismatches = 0;
        std::int64_t firstMismatch = -1;
        int firstMismatchChannel = -1;
        double maximum = 0, squares = 0;
        for (int start = 0; start < 264600; start += 512) {
            const int hostEnd = std::min(start + 512, 264600);
            int position = start;
            while (position < hostEnd) {
                const int end = position < 88200 ? std::min(hostEnd, 88200) : hostEnd;
                const int count = end - position;
                block.setSize(2, count, false, false, true);
                block.clear();
                midi.clear();
                if (position == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 24, static_cast<juce::uint8>(100)), 0);
                if (position == 88200) midi.addEvent(juce::MidiMessage::noteOff(1, 24), 0);
                processWithHostFpMode(plugin, block, midi, flushSubnormals);
                for (int channel = 0; channel < 2; ++channel) {
                    for (int sample = 0; sample < count; ++sample) {
                        const auto actual = block.getSample(channel, sample);
                        require(std::isfinite(actual), "Non-finite plugin preset output");
                        const auto wantedBits = expected[static_cast<size_t>(position + sample) * referenceChannels
                                                         + (full ? channel : 0)];
                        float wanted;
                        std::memcpy(&wanted, &wantedBits, 4);
                        require(std::isfinite(wanted), "Non-finite reference sample");
                        if (floatBits(actual) != wantedBits) {
                            ++mismatches;
                            if (firstMismatch < 0 || position + sample < firstMismatch) {
                                firstMismatch = position + sample;
                                firstMismatchChannel = channel;
                            }
                        }
                        const double error = static_cast<double>(actual) - wanted;
                        maximum = std::max(maximum, std::abs(error));
                        squares += error * error;
                    }
                }
                position = end;
            }
        }
        plugin.releaseResources();
        totalMismatches += mismatches;
        totalSquareError += squares;
        totalMaximum = std::max(totalMaximum, maximum);
        csv << format << ',' << (flushSubnormals ? 1 : 0) << ',' << preset << ",264600,529200," << mismatches << ',' << firstMismatch
            << ',' << maximum << ',' << std::sqrt(squares / 529200.0) << ',' << referenceChannels
            << ',' << firstMismatchChannel << '\n';
        if (mismatches)
            std::cout << format << " preset=" << preset << " mismatched_channel_samples=" << mismatches
                      << " first_frame=" << firstMismatch << " max_error=" << maximum << '\n';
    }
    std::cout << format << (full ? " full stereo" : " dry mono") << " incoming host FZ=" << (flushSubnormals ? 1 : 0)
              << " actual bundle preset suite: 128 presets, 67737600 channel samples; bit_mismatches="
              << totalMismatches << " max_abs_error=" << totalMaximum
              << " rms_error=" << std::sqrt(totalSquareError / 67737600.0) << '\n';
    return totalMismatches;
}

void registerLocalAU(const juce::File& bundle)
{
    // Registration lasts only in this process. No bundle or registry is installed.
    auto* library = dlopen(bundle.getChildFile("Contents/MacOS/PlastiCZ Recovery").getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) throw std::runtime_error(dlerror());
    auto factory = reinterpret_cast<AudioComponentFactoryFunction>(dlsym(library, "PlastiCZ_RecoveryAUFactory"));
    require(factory != nullptr, "AU factory export missing");
    AudioComponentDescription desc{};
    desc.componentType = 'aumu';
    desc.componentSubType = 'PzR1';
    desc.componentManufacturer = 'Rcvr';
    require(AudioComponentRegister(&desc, CFSTR("Recovery Project: PlastiCZ Recovery"), 0x000100, factory) != nullptr,
            "Process-local AU registration failed");
}

std::uint64_t testOversamplingSuite(juce::AudioPluginInstance& plugin, const juce::String& format,
                                   const juce::File& references, std::ofstream& csv, bool flushSubnormals)
{
    juce::MemoryBlock audio;
    require(references.getChildFile("audio.f32").loadFileAsData(audio), "Missing 4x audio fixtures");
    std::ifstream commands(references.getChildFile("commands.txt").getFullPathName().toStdString());
    std::string line, name;
    std::getline(commands, line);
    require(line == "PLASTICZ_OVERSAMPLING_ORACLE 1", "Wrong 4x fixture schema");
    bool active = false;
    int cases = 0;
    double rate = 44100, maximum = 0;
    std::uint64_t mismatches = 0, samples = 0, caseBad = 0, caseSamples = 0;
    juce::MidiBuffer pending;
    while (std::getline(commands, line)) {
        std::istringstream row(line);
        std::string action;
        row >> action;
        if (action == "case") {
            int full;
            require(bool(row >> name >> rate >> full), "Invalid 4x case");
            active = full != 0;
            if (active) {
                ++cases; caseBad = caseSamples = 0;
                plugin.setCurrentProgram(0);
                pending.clear();
            }
        } else if (action == "downsampler") active = false;
        else if (!active) continue;
        else if (action == "parameters") {
            for (int i = 0; i < 38; ++i) {
                float value;
                require(bool(row >> value), "Incomplete 4x parameters");
                plugin.getParameters()[i]->setValueNotifyingHost(value);
            }
        } else if (action == "prepare") plugin.prepareToPlay(rate, 8192);
        else if (action == "parameter") {
            int index; float value;
            require(bool(row >> index >> value) && index >= 0 && index < 38, "Invalid 4x parameter event");
            plugin.getParameters()[index]->setValueNotifyingHost(value);
        } else if (action == "note_on") {
            int note; float velocity;
            require(bool(row >> note >> velocity), "Invalid 4x note-on");
            pending.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(std::lround(velocity * 127))), 0);
        } else if (action == "note_off") {
            int note; require(bool(row >> note), "Invalid 4x note-off");
            pending.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        } else if (action == "host") {
            int count; size_t offset, stateOffset;
            require(bool(row >> count >> offset >> stateOffset) && count >= 0 && count <= 8192,
                    "Invalid 4x render event");
            require(offset + static_cast<size_t>(count) * 8 <= audio.getSize(), "4x fixture range invalid");
            // Keep events for the next nonempty host block; no time passes in an empty block.
            if (count == 0) continue;
            juce::AudioBuffer<float> buffer(2, count);
            processWithHostFpMode(plugin, buffer, pending, flushSubnormals);
            pending.clear();
            for (int i = 0; i < count; ++i) for (int channel = 0; channel < 2; ++channel) {
                const auto expectedBits = word(static_cast<const char*>(audio.getData()) + offset + (i * 2 + channel) * 4);
                const auto actual = buffer.getSample(channel, i);
                require(std::isfinite(actual), "Nonfinite actual 4x plugin output");
                if (floatBits(actual) != expectedBits) { ++mismatches; ++caseBad; }
                float expected; std::memcpy(&expected, &expectedBits, 4);
                maximum = std::max(maximum, std::abs(double(actual) - expected));
                ++samples; ++caseSamples;
            }
        } else if (action == "end") {
            csv << format << ',' << flushSubnormals << ',' << name << ',' << caseSamples << ',' << caseBad << '\n';
            if (caseBad) std::cerr << format << " " << name << " mismatches=" << caseBad << '\n';
            plugin.releaseResources();
            active = false;
        } else throw std::runtime_error("Unexpected full-path 4x action: " + action);
    }
    require(cases == 11 && samples > 0, "Incomplete actual-bundle 4x comparison");
    std::cout << format << " actual 4x bundle, host FZ=" << flushSubnormals << ": " << cases
              << " effect cases, " << samples << " channel samples; bit_mismatches=" << mismatches
              << " max_abs_error=" << maximum << '\n';
    return mismatches;
}

std::uint64_t testBundle(juce::AudioPluginFormat& format, const juce::String& identifier,
                        const juce::File& references, std::ofstream& csv, bool full, bool oversampling)
{
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, identifier);
    require(descriptions.size() == 1, "Expected one plugin description");
    juce::String error;
    auto instance = format.createInstanceFromDescription(*descriptions[0], 44100.0, 1024, error);
    if (instance == nullptr) throw std::runtime_error(error.toStdString());
    require(instance->getNumPrograms() == 128, "Expected 128 recovered programs");
    require(instance->getParameters().size() >= 38, "Expected 38 recovered parameters");
    require(instance->getTotalNumOutputChannels() == 2, "Expected stereo output");
    instance->setCurrentProgram(0);
    instance->prepareToPlay(44100.0, 1024);
    if (full) {
        // JUCE's AU wrapper publishes latency changes through an AsyncUpdater;
        // service the same main run loop a real host runs before inspecting it.
        for (int attempt = 0; attempt < 50 && instance->getLatencySamples() != 44; ++attempt)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        if (instance->getLatencySamples() != 44)
            std::cerr << format.getName() << " reported latency=" << instance->getLatencySamples() << '\n';
        require(instance->getLatencySamples() == 44, "Host did not receive the recovered 44-sample output latency");
    }

    // The full original output stage can emit a no-note bias. Capture that
    // deterministic baseline before checking that MIDI does not affect it early.
    juce::AudioBuffer<float> baseline(2, 128);
    baseline.clear();
    if (full) {
        auto silent = format.createInstanceFromDescription(*descriptions[0], 44100.0, 1024, error);
        require(silent != nullptr, "Cannot instantiate no-note baseline");
        silent->setCurrentProgram(0);
        silent->prepareToPlay(44100.0, 1024);
        juce::MidiBuffer noMidi;
        silent->processBlock(baseline, noMidi);
        silent->releaseResources();
    }
    juce::AudioBuffer<float> buffer(2, 1024);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 128);
    instance->processBlock(buffer, midi);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 128; ++sample)
            require(floatBits(buffer.getSample(channel, sample)) == floatBits(baseline.getSample(channel, sample)),
                    "MIDI event changed output before its timestamp");
    float peak = buffer.getMagnitude(0, 1024);
    for (int block = 0; block < 32; ++block)
    {
        midi.clear();
        instance->processBlock(buffer, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                require(std::isfinite(buffer.getSample(channel, i)), "Non-finite audio output");
        peak = std::max(peak, buffer.getMagnitude(0, 1024));
    }
    require(peak > 0.0001f, "No audible waveform was rendered");

    instance->setCurrentProgram(37);
    juce::MemoryBlock saved;
    instance->getStateInformation(saved);
    require(saved.getSize() > 100, "State serialization is empty");
    const auto savedName = instance->getProgramName(37);
    auto* parameter = instance->getParameters()[0];
    const auto previous = parameter->getValue();
    parameter->setValueNotifyingHost(0.123456f);
    instance->setCurrentProgram(10);
    instance->setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    require(instance->getCurrentProgram() == 37, "Current program did not restore");
    require(instance->getProgramName(37) == savedName, "Program name did not restore");
    require(std::abs(parameter->getValue() - previous) < 0.000001f, "Parameter did not restore");

    midi.clear();
    midi.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    instance->processBlock(buffer, midi);
    if (!full) require(buffer.getMagnitude(0, 1024) == 0.0f, "All-sound-off left an active dry voice");
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            require(std::isfinite(buffer.getSample(channel, sample)), "Non-finite output after all-sound-off");
    instance->releaseResources();
    std::cout << format.getName() << ": loaded native bundle; 128 programs; MIDI sample offset, audio, state restore and all-sound-off dispatch passed. Peak="
              << peak << '\n';
    if (! references.isDirectory()) return 0;
    if (oversampling) return testOversamplingSuite(*instance, format.getName(), references, csv, false)
                           + testOversamplingSuite(*instance, format.getName(), references, csv, true);
    auto differences = testPresetSuite(*instance, format.getName(), references, csv, false, full);
    differences += testPresetSuite(*instance, format.getName(), references, csv, true, full);
    return differences;
}
}

int main(int argc, char** argv)
{
    try
    {
        require(argc >= 3, "Usage: check_plugin_host <plugin.vst3> <plugin.component> [--full|--4x] [reference-dir comparison.csv]");
        const bool oversampling = argc >= 4 && std::string(argv[3]) == "--4x";
        const bool full = oversampling || (argc >= 4 && std::string(argv[3]) == "--full");
        const int firstOptional = full ? 4 : 3;
        require(argc == firstOptional || argc == firstOptional + 2, "Unexpected host-test arguments");
        juce::ScopedJuceInitialiser_GUI gui;
        juce::File references;
        std::ofstream csv;
        if (argc == firstOptional + 2) {
            references = juce::File(argv[firstOptional]);
            require(references.isDirectory(), "Reference directory is missing");
            csv.open(argv[firstOptional + 1]);
            require(static_cast<bool>(csv), "Cannot write host comparison CSV");
            csv << (oversampling ? "format,host_flush_subnormals,case,channel_samples,bit_mismatches\n"
                : "format,host_flush_subnormals,preset,frames,channel_samples,bit_mismatches,first_mismatch_frame,max_abs_error,rms_error,reference_channels,first_mismatch_channel\n")
                << std::setprecision(17);
        }
        juce::VST3PluginFormat vst3;
        auto differences = testBundle(vst3, juce::File(argv[1]).getFullPathName(), references, csv, full, oversampling);
        registerLocalAU(juce::File(argv[2]));
        juce::AudioUnitPluginFormat au;
        differences += testBundle(au, "AudioUnit:Synths/aumu,PzR1,Rcvr", references, csv, full, oversampling);
        return differences ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
