#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cstring>

namespace
{
// Native host state carries the editor preference separately from the original
// bank. Legacy raw chunks are still accepted unchanged by setStateInformation.
constexpr char nativeStateMagic[] = "PZUI0001";
constexpr size_t nativeStateHeaderSize = 16;
constexpr const char* originalNames[] = {
    "DCW_ATK", "DCW_DCY", "DCW_SUS", "DCW_REL", "DCW_DPT", "DCW_VEL", "dummy1", "dummy2",
    "OSC_1A", "OSC_1B", "OSC_2A", "OSC_2B", "OSC_1SE", "OSC_1FI", "OSC_2SE", "OSC_2FI",
    "MIX_RNG", "MIX_OS1", "MIX_OS2", "dummy3",
    "AMP_ATK", "AMP_DCY", "AMP_SUS", "AMP_REL", "AMP_DPT", "AMP_VEL", "dummy4", "dummy5",
    "FX1_TPE", "FX1_MIX", "FX1_PA1", "FX1_PA2", "PB RNG", "POR SPD", "VIB SPD", "VOICES", "FX BYPS", "4X OVER"
};
}

juce::String PlastiCZProcessor::parameterID(int index)
{
    // Numeric original order remains stable even if display labels improve.
    return "plasticz_p" + juce::String(index).paddedLeft('0', 2);
}

const char* PlastiCZProcessor::parameterName(int index) { return originalNames[index]; }

juce::AudioProcessorValueTreeState::ParameterLayout PlastiCZProcessor::parameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto& factory = plasticz::factoryBank();
    for (int i = 0; i < static_cast<int>(plasticz::parameterCount); ++i)
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{parameterID(i), 1}, parameterName(i),
            juce::NormalisableRange<float>(0.0f, 1.0f), factory[0].values[static_cast<size_t>(i)],
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction([](float value, int) { return juce::String(value, 3); })
                .withValueFromStringFunction([](const juce::String& value) { return value.getFloatValue(); })));
    return layout;
}

PlastiCZProcessor::PlastiCZProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PlastiCZRecoveryParameters", parameterLayout()),
      bank(plasticz::factoryBank())
{
    for (int i = 0; i < static_cast<int>(plasticz::parameterCount); ++i)
        rawParameters[static_cast<size_t>(i)] = parameters.getRawParameterValue(parameterID(i));
    audioParameters = readParameters();
}

plasticz::Parameters PlastiCZProcessor::readParameters() const
{
    plasticz::Parameters result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = rawParameters[i]->load(std::memory_order_relaxed);
    return result;
}

bool PlastiCZProcessor::captureAudioParameters()
{
    const auto before = parameterGeneration.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return false;
    const auto candidate = readParameters();
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before != parameterGeneration.load(std::memory_order_acquire)) return false;
    audioParameters = candidate;
    const bool newProgram = before != audioParameterGeneration;
    audioParameterGeneration = before;
    return newProgram;
}

void PlastiCZProcessor::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    engine.prepare(sampleRate, maximumBlockSize);
    setLatencySamples(engine.latencySamples());
    captureAudioParameters();
    engine.setParameters(audioParameters);
    keyboardState.reset();
}

bool PlastiCZProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layout.getMainInputChannelSet().isDisabled();
}

void PlastiCZProcessor::handleMidi(const juce::MidiMessage& message)
{
    midiActivityCount.fetch_add(1, std::memory_order_relaxed);
    if (message.isNoteOn()) engine.noteOn(message.getNoteNumber(), message.getFloatVelocity());
    else if (message.isNoteOff()) engine.noteOff(message.getNoteNumber());
    else if (message.isPitchWheel()) engine.pitchBend(static_cast<float>(message.getPitchWheelValue()) / 16383.0f);
    else if (message.isController() && message.getControllerNumber() == 1)
        engine.modWheel(static_cast<float>(message.getControllerValue()) / 127.0f);
    else if (message.isController() && message.getControllerNumber() == 7)
        engine.setVolume(static_cast<float>(message.getControllerValue()) / 127.0f);
    else if (message.isController() && message.getControllerNumber() == 11)
        engine.setExpression(static_cast<float>(message.getControllerValue()) / 127.0f);
    else if (message.isAllSoundOff()) engine.allNotesOff(true);
    else if (message.isAllNotesOff()) engine.allNotesOff(false);
}

void PlastiCZProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Retain the legacy SSE path's gradual underflow even when a host flushes
    // subnormals. The JUCE guard restores the host's original FP register.
    juce::ScopedNoDenormals restoreHostFpState;
    juce::FloatVectorOperations::disableDenormalisedNumberSupport(false);
    buffer.clear();
    if (buffer.getNumChannels() < 2) return;

    // A program load changes 38 atomic values. Reuse the previous complete
    // snapshot if a load overlaps this block; the audio thread never waits.
    const bool newProgram = captureAudioParameters();
    engine.setParameters(audioParameters);
    // Consume the reset with the same complete generation as its parameters.
    // A later concurrent load will be picked up and reset on the next block.
    if (newProgram) engine.reset();
    if (panicRequested.exchange(false, std::memory_order_acq_rel))
    {
        engine.allNotesOff(true);
        keyboardState.reset();
    }
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
            if (const auto bpm = position->getBpm()) engine.setTempo(*bpm);

    keyboardState.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);
    int rendered = 0;
    for (const auto event : midi)
    {
        const int at = juce::jlimit(rendered, buffer.getNumSamples(), event.samplePosition);
        if (at > rendered)
            engine.render(buffer.getWritePointer(0, rendered), buffer.getWritePointer(1, rendered), at - rendered);
        handleMidi(event.getMessage());
        rendered = at;
    }
    if (rendered < buffer.getNumSamples())
        engine.render(buffer.getWritePointer(0, rendered), buffer.getWritePointer(1, rendered), buffer.getNumSamples() - rendered);
    for (int channel = 0; channel < 2; ++channel)
        latestOutputPeak[channel].store(buffer.getMagnitude(channel, 0, buffer.getNumSamples()), std::memory_order_relaxed);
    midi.clear();
}

int PlastiCZProcessor::getCurrentProgram()
{
    const juce::ScopedLock lock(stateLock);
    return currentProgram;
}

void PlastiCZProcessor::saveCurrentProgramLocked() { bank[static_cast<size_t>(currentProgram)].values = readParameters(); }

void PlastiCZProcessor::applyCurrentProgramLocked()
{
    parameterGeneration.fetch_add(1, std::memory_order_acq_rel);
    const auto& values = bank[static_cast<size_t>(currentProgram)].values;
    for (int i = 0; i < static_cast<int>(plasticz::parameterCount); ++i)
        parameters.getParameter(parameterID(i))->setValueNotifyingHost(values[static_cast<size_t>(i)]);
    parameterGeneration.fetch_add(1, std::memory_order_release);
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
    sendChangeMessage();
}

void PlastiCZProcessor::setCurrentProgram(int index)
{
    const juce::ScopedLock lock(stateLock);
    if (index < 0 || index >= getNumPrograms()) return;
    saveCurrentProgramLocked();
    currentProgram = index;
    applyCurrentProgramLocked();
}

const juce::String PlastiCZProcessor::getProgramName(int index)
{
    const juce::ScopedLock lock(stateLock);
    if (index < 0 || index >= getNumPrograms()) return {};
    const auto& name = bank[static_cast<size_t>(index)].name;
    const auto end = std::find(name.begin(), name.end(), '\0');
    juce::String display;
    for (auto ch = name.begin(); ch != end; ++ch)
        display += juce::String::charToString(static_cast<juce::juce_wchar>(static_cast<unsigned char>(*ch)));
    return display;
}

void PlastiCZProcessor::changeProgramName(int index, const juce::String& name)
{
    const juce::ScopedLock lock(stateLock);
    if (index < 0 || index >= getNumPrograms()) return;
    auto& target = bank[static_cast<size_t>(index)].name;
    target.fill(0);
    // Legacy names are single-byte text; use the same encoding when writing
    // and displaying them. Unsupported characters get a visible replacement.
    size_t at = 0;
    for (auto character : name)
    {
        if (at == target.size() - 1) break;
        target[at++] = static_cast<char>(character <= 255 ? character : '?');
    }
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
    sendChangeMessage();
}

void PlastiCZProcessor::getStateInformation(juce::MemoryBlock& result)
{
    const juce::ScopedLock lock(stateLock);
    saveCurrentProgramLocked();
    const auto encoded = plasticz::encodeState(bank, currentProgram);
    result.setSize(0);
    juce::MemoryOutputStream output(result, false);
    output.write(nativeStateMagic, 8);
    output.writeInt(static_cast<int>(encoded.size()));
    output.writeInt(isClassicEditor() ? 1 : 0);
    output.write(encoded.data(), encoded.size());
}

void PlastiCZProcessor::setClassicEditor(bool classic)
{
    if (classicEditor.exchange(classic, std::memory_order_relaxed) == classic) return;
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
    sendChangeMessage();
}

void PlastiCZProcessor::applyLoadedStateLocked(const plasticz::LoadedState& state)
{
    bank = state.bank;
    currentProgram = state.currentProgram;
    stateError.clear();
    applyCurrentProgramLocked();
}

void PlastiCZProcessor::setStateInformation(const void* data, int bytes)
{
    if (data == nullptr || bytes <= 0) return;
    plasticz::LoadedState loaded;
    std::string error;
    auto* bankData = static_cast<const std::uint8_t*>(data);
    auto bankBytes = static_cast<size_t>(bytes);
    bool classic = isClassicEditor();
    if (bankBytes >= 8 && std::memcmp(bankData, nativeStateMagic, 8) == 0)
    {
        if (bankBytes < nativeStateHeaderSize)
            error = "Native state header is incomplete";
        else
        {
            const auto payloadSize = juce::ByteOrder::littleEndianInt(bankData + 8);
            const auto flags = juce::ByteOrder::littleEndianInt(bankData + 12);
            if (payloadSize != bankBytes - nativeStateHeaderSize || flags > 1)
                error = "Native state length or editor setting is invalid";
            else
            {
                classic = flags != 0;
                bankData += nativeStateHeaderSize;
                bankBytes -= nativeStateHeaderSize;
            }
        }
    }
    if (!error.empty() || !plasticz::decodeState(bankData, bankBytes, loaded, error))
    {
        const juce::ScopedLock lock(stateLock);
        stateError = error;
        sendChangeMessage();
        return;
    }
    const juce::ScopedLock lock(stateLock);
    classicEditor.store(classic, std::memory_order_relaxed);
    applyLoadedStateLocked(loaded);
}

bool PlastiCZProcessor::importPreset(const juce::File& file, juce::String& error)
{
    if (file.getSize() > 4 * 1024 * 1024)
    {
        error = "This file is too large to be a PlastiCZ preset or bank.";
        return false;
    }
    juce::MemoryBlock content;
    if (!file.loadFileAsData(content)) { error = "The selected file could not be read."; return false; }
    const juce::ScopedLock lock(stateLock);
    saveCurrentProgramLocked();
    plasticz::LoadedState current{bank, currentProgram}, loaded;
    std::string detail;
    if (!plasticz::loadPresetFile(content.getData(), content.getSize(), current, loaded, detail))
    {
        error = detail;
        return false;
    }
    applyLoadedStateLocked(loaded);
    return true;
}

bool PlastiCZProcessor::exportPreset(const juce::File& file, bool entireBank, juce::String& error)
{
    juce::MemoryBlock content;
    {
        const juce::ScopedLock lock(stateLock);
        saveCurrentProgramLocked();
        const auto raw = entireBank ? plasticz::encodeState(bank, currentProgram) : std::vector<std::uint8_t>{};
        juce::MemoryOutputStream stream(content, false);
        const int size = entireBank ? 160 + static_cast<int>(raw.size()) : 56 + 38 * 4;
        stream.write("CcnK", 4);
        stream.writeIntBigEndian(size - 8);
        stream.write(entireBank ? "FBCh" : "FxCk", 4);
        stream.writeIntBigEndian(1); // VST preset-container version.
        stream.writeIntBigEndian(static_cast<int>(plasticz::legacyPluginId));
        stream.writeIntBigEndian(0); // No claim of an original vendor build version.
        stream.writeIntBigEndian(entireBank ? 128 : 38);
        if (entireBank)
        {
            stream.writeRepeatedByte(0, 128);
            stream.writeIntBigEndian(static_cast<int>(raw.size()));
            stream.write(raw.data(), raw.size());
        }
        else
        {
            const auto& program = bank[static_cast<size_t>(currentProgram)];
            stream.write(program.name.data(), 28); // Standard FXP name-field width.
            for (float value : program.values)
            {
                std::uint32_t bits;
                std::memcpy(&bits, &value, sizeof(bits));
                stream.writeIntBigEndian(static_cast<int>(bits));
            }
        }
    }
    if (!file.replaceWithData(content.getData(), content.getSize()))
    {
        error = "The preset file could not be saved.";
        return false;
    }
    error.clear();
    return true;
}

void PlastiCZProcessor::restoreFactoryBank()
{
    const juce::ScopedLock lock(stateLock);
    bank = plasticz::factoryBank();
    currentProgram = 0;
    applyCurrentProgramLocked();
}

juce::String PlastiCZProcessor::lastStateError() const
{
    const juce::ScopedLock lock(stateLock);
    return stateError;
}

juce::AudioProcessorEditor* PlastiCZProcessor::createEditor() { return new PlastiCZEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PlastiCZProcessor(); }
