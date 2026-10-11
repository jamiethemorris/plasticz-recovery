#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Presets.h"
#include "SynthEngine.h"
#include <array>
#include <atomic>

class PlastiCZProcessor final : public juce::AudioProcessor,
                               public juce::ChangeBroadcaster
{
public:
    PlastiCZProcessor();
    ~PlastiCZProcessor() override = default;
    const juce::String getName() const override { return "PlastiCZ Recovery"; }
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 20.0; }
    int getNumPrograms() override { return static_cast<int>(plasticz::programCount); }
    int getCurrentProgram() override;
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    bool importPreset(const juce::File&, juce::String& error);
    bool exportPreset(const juce::File&, bool entireBank, juce::String& error);
    void restoreFactoryBank();
    void requestPanic() { panicRequested.store(true, std::memory_order_release); }
    juce::String lastStateError() const;
    bool isClassicEditor() const noexcept { return classicEditor.load(std::memory_order_relaxed); }
    void setClassicEditor(bool);
    float outputPeak(int channel = 0) noexcept {
        return latestOutputPeak[channel == 1 ? 1 : 0].exchange(0.0f, std::memory_order_relaxed);
    }
    unsigned midiActivity() const noexcept { return midiActivityCount.load(std::memory_order_relaxed); }

    static juce::String parameterID(int index);
    static const char* parameterName(int index);
    juce::AudioProcessorValueTreeState parameters;
    juce::MidiKeyboardState keyboardState;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout();
    plasticz::Parameters readParameters() const;
    bool captureAudioParameters();
    void saveCurrentProgramLocked();
    void applyCurrentProgramLocked();
    void applyLoadedStateLocked(const plasticz::LoadedState&);
    void handleMidi(const juce::MidiMessage&);

    mutable juce::CriticalSection stateLock;
    plasticz::Bank bank;
    int currentProgram = 0;
    juce::String stateError;
    std::array<std::atomic<float>*, plasticz::parameterCount> rawParameters{};
    std::atomic<unsigned> parameterGeneration{0};
    std::atomic<bool> panicRequested{false};
    unsigned audioParameterGeneration = 0;
    std::atomic<bool> classicEditor{true};
    std::array<std::atomic<float>, 2> latestOutputPeak{{0.0f, 0.0f}};
    std::atomic<unsigned> midiActivityCount{0};
    plasticz::Parameters audioParameters{};
    plasticz::SynthEngine engine;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlastiCZProcessor)
};
