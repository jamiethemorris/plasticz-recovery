#include "PluginProcessor.h"
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
void dispatch() {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, false);
}
juce::Component* find(juce::Component& parent, const juce::String& id) {
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* result = find(*child, id)) return result;
    return nullptr;
}
template<class Component> Component& control(juce::Component& root, const juce::String& id) {
    auto* found = dynamic_cast<Component*>(find(root, id));
    if (!found) throw std::runtime_error("Missing editor control: " + id.toStdString());
    return *found;
}
plasticz::Parameters values(PlastiCZProcessor& processor) {
    plasticz::Parameters result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = processor.parameters.getRawParameterValue(PlastiCZProcessor::parameterID(static_cast<int>(i)))->load();
    return result;
}
void unchanged(const plasticz::Parameters& expected, PlastiCZProcessor& processor) {
    const auto actual = values(processor);
    require(std::memcmp(expected.data(), actual.data(), sizeof(actual)) == 0,
            "Editor display or mode change altered stored parameter precision");
}
void set(PlastiCZProcessor& processor, int index, float value) {
    processor.parameters.getParameter(PlastiCZProcessor::parameterID(index))->setValueNotifyingHost(value);
}
void screenshot(juce::AudioProcessorEditor& editor, const juce::File& file) {
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    juce::FileOutputStream stream(file);
    require(!stream.failedToOpen(), "Cannot open screenshot output");
    stream.setPosition(0);
    stream.truncate();
    juce::PNGImageFormat png;
    require(png.writeImageToStream(image, stream), "Cannot encode editor screenshot");
    std::cout << "Editor snapshot: " << file.getFullPathName() << '\n';
}
void stateChecks(const juce::File& directory) {
    PlastiCZProcessor processor;
    processor.setCurrentProgram(37);
    processor.changeProgramName(37, juce::String::fromUTF8("Caf\xc3\xa9"));
    require(processor.getProgramName(37) == juce::String::fromUTF8("Caf\xc3\xa9"), "Legacy name encoding does not round-trip");
    processor.setClassicEditor(false);
    set(processor, 28, .527419f);
    set(processor, 37, .731234f);
    const auto expected = values(processor);
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    processor.setClassicEditor(true);
    processor.setCurrentProgram(0);
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    require(!processor.isClassicEditor() && processor.getCurrentProgram() == 37,
            "Host state lost interface mode or selected program");
    unchanged(expected, processor);

    // Malformed native headers must not partially mutate mode, program or sound.
    auto malformed = saved;
    static_cast<unsigned char*>(malformed.getData())[12] = 7;
    processor.setStateInformation(malformed.getData(), static_cast<int>(malformed.getSize()));
    require(processor.lastStateError().isNotEmpty(), "Malformed native flags accepted");
    unchanged(expected, processor);
    require(!processor.isClassicEditor() && processor.getCurrentProgram() == 37, "Malformed state mutated selection");
    processor.setStateInformation(saved.getData(), 10);
    require(processor.lastStateError().isNotEmpty(), "Truncated native header accepted");
    unchanged(expected, processor);

    // Old Recovery 0.1/0.2 and original raw host chunks retain full compatibility.
    auto bank = plasticz::factoryBank();
    bank[12].values[28] = .687543f;
    const auto legacy = plasticz::encodeState(bank, 12);
    processor.setStateInformation(legacy.data(), static_cast<int>(legacy.size()));
    require(processor.lastStateError().isEmpty() && processor.getCurrentProgram() == 12,
            "Legacy raw host state no longer loads");
    unchanged(bank[12].values, processor);
    require(!processor.isClassicEditor(), "Legacy preset overwrote the independent interface preference");

    // The original load/save controls must produce real interoperable files.
    juce::String error;
    const auto fxp = directory.getChildFile("roundtrip.fxp");
    const auto fxb = directory.getChildFile("roundtrip.fxb");
    const auto program = values(processor);
    juce::MemoryBlock bankBefore;
    processor.getStateInformation(bankBefore);
    require(processor.exportPreset(fxp, false, error), "FXP export failed");
    require(processor.exportPreset(fxb, true, error), "FXB export failed");
    processor.setCurrentProgram(17);
    require(processor.importPreset(fxp, error), "Exported FXP cannot be imported");
    require(processor.getCurrentProgram() == 17, "FXP import changed selected slot");
    unchanged(program, processor);
    require(processor.importPreset(fxb, error), "Exported FXB cannot be imported");
    juce::MemoryBlock bankAfter;
    processor.getStateInformation(bankAfter);
    require(bankBefore == bankAfter, "FXB round-trip altered full bank names, parameters or selected program");
}
void verify(const juce::File& directory) {
    require(directory.createDirectory().wasOk(), "Cannot create screenshot directory");
    stateChecks(directory);
    PlastiCZProcessor processor;
    // Deliberately non-grid values expose unwanted menu/toggle quantization.
    for (int i = 0; i < 38; ++i) set(processor, i, static_cast<float>((i * 1729 + 3157) % 10000) / 10000.0f);
    const auto initial = values(processor);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    dispatch();
    unchanged(initial, processor);
    auto& settings = control<juce::Button>(*editor, "settings-toggle");
    auto& interfaceMode = control<juce::ComboBox>(*editor, "interface-mode");
    require(!settings.getToggleState() && !interfaceMode.isVisible(), "Settings must start hidden");
    require(editor->getWidth() == 1090 && editor->getHeight() == 468, "Default editor is not compact");
    // Expanding/collapsing must preserve scale and all stored parameter bits.
    for (int width : {872, 1090, 1744}) {
        editor->setSize(width, juce::roundToInt(width * 374.0 / 872));
        settings.triggerClick();
        dispatch();
        require(settings.getToggleState() && interfaceMode.isVisible(), "Settings did not open");
        require(editor->getWidth() == width && editor->getHeight() == juce::roundToInt(width * 442.0 / 872),
                "Opening settings changed scale or clipped the drawer");
        unchanged(initial, processor);
        settings.triggerClick();
        dispatch();
        require(!settings.getToggleState() && !interfaceMode.isVisible(), "Settings did not close");
        require(!control<juce::Button>(*editor, "plasticz_p37").isVisible(), "Hidden 4x control remains focusable");
        require(editor->getWidth() == width && editor->getHeight() == juce::roundToInt(width * 374.0 / 872),
                "Closing settings changed scale");
        unchanged(initial, processor);
    }
    editor->setSize(1090, 468);
    settings.triggerClick();
    dispatch();
    for (const bool classic : {true, false}) {
        auto& mode = control<juce::ComboBox>(*editor, "interface-mode");
        mode.setSelectedId(classic ? 1 : 2, juce::sendNotificationSync);
        dispatch();
        require(processor.isClassicEditor() == classic, "Interface selector did not change processor preference");
        unchanged(initial, processor);
    }
    for (const bool classic : {true, false}) {
        processor.setClassicEditor(classic);
        dispatch();
        auto& mode = control<juce::ComboBox>(*editor, "interface-mode");
        require(mode.getSelectedId() == (classic ? 1 : 2), "Interface selector did not reflect restored mode");
        for (int index : {8, 9, 10, 11}) {
            for (int wave = 0; wave < 8; ++wave) {
                auto& button = control<juce::Button>(*editor, PlastiCZProcessor::parameterID(index) + "_wave_" + juce::String(wave));
                button.triggerClick();
                dispatch();
                require(static_cast<int>(values(processor)[index] * 7.989999771118164f) == wave,
                        "Waveform button selected the wrong oscillator shape");
            }
            set(processor, index, .527419f);
            dispatch();
            control<juce::Button>(*editor, PlastiCZProcessor::parameterID(index) + "_wave_4").triggerClick();
            dispatch();
            require(values(processor)[index] == .527419f, "Re-clicking an already-selected wave quantized its legacy value");
        }
        for (int index : {0,1,2,3,4,5,12,13,14,15,16,17,18,20,21,22,23,24,25,29,30,31,32,33,34,35}) {
            set(processor, index, .4f);
            dispatch();
            auto* slider = find(*editor, PlastiCZProcessor::parameterID(index));
            require(slider && slider->keyPressed(juce::KeyPress(juce::KeyPress::upKey)), "Continuous slider is not keyboard operable");
            require(values(processor)[index] > .4f, "Continuous control is disconnected from its parameter");
        }
        auto& effect = control<juce::ComboBox>(*editor, "plasticz_p28");
        require(effect.isEnabled() && effect.getNumItems() == 11, "Expected 11 selectable effect choices");
        for (int index = 0; index < 11; ++index) {
            effect.setSelectedItemIndex(index, juce::sendNotificationSync);
            dispatch();
            require(static_cast<int>(values(processor)[28] * 10.989999771118164f) == index,
                    "Effect menu failed to select the requested DSP algorithm");
        }
        set(processor, 28, .527419f);
        dispatch();
        require(effect.getSelectedItemIndex() == 5 && values(processor)[28] == .527419f,
                "Automated FX type is displayed incorrectly or quantized");
        for (const int parameter : {36, 37}) {
            auto& toggle = control<juce::Button>(*editor, PlastiCZProcessor::parameterID(parameter));
            require(toggle.isEnabled(), "FX bypass or 4x toggle is disabled");
            toggle.setToggleState(false, juce::sendNotificationSync);
            require(values(processor)[parameter] < .5f, "Toggle failed to disable its parameter");
            toggle.setToggleState(true, juce::sendNotificationSync);
            require(values(processor)[parameter] >= .5f, "Toggle failed to enable its parameter");
            set(processor, parameter, .731234f);
            dispatch();
            require(toggle.getToggleState() && values(processor)[parameter] == .731234f,
                    "Automated toggle value was quantized or displayed incorrectly");
        }
        for (int preset = 0; preset < 128; ++preset) {
            processor.setCurrentProgram(preset);
            const auto expected = values(processor);
            dispatch();
            unchanged(expected, processor);
            require(effect.getSelectedItemIndex() == static_cast<int>(expected[28] * 10.989999771118164f),
                    "Preset did not update the visible effect choice");
        }
        processor.restoreFactoryBank();
        dispatch();
        screenshot(*editor, directory.getChildFile(classic ? "editor-classic-settings.png" : "editor-modern-settings.png"));
        const auto beforeCollapse = values(processor);
        settings.triggerClick();
        dispatch();
        require(!interfaceMode.isVisible(), "Settings remained visible in selected skin");
        unchanged(beforeCollapse, processor);
        screenshot(*editor, directory.getChildFile(classic ? "editor-classic.png" : "editor-modern.png"));
        settings.triggerClick();
        dispatch();
    }
    const auto savedValues = values(processor);
    editor.reset();
    editor.reset(processor.createEditor());
    dispatch();
    unchanged(savedValues, processor);
    require(control<juce::ComboBox>(*editor, "interface-mode").getSelectedId() == 2,
            "Reopening editor lost the selected interface");
    require(!control<juce::ComboBox>(*editor, "interface-mode").isVisible(), "Reopened editor must start compact");
    std::cout << "Editor verification passed: both interfaces, settings drawer visibility/resize/scale, 11 FX choices, 32 waveform buttons, 26 continuous controls, bypass/4x, automation, all 128 preset displays, exact stored values, host state, legacy state and FXP/FXB export.\n";
}
}
int main(int argc, char** argv) {
    try {
        juce::ScopedJuceInitialiser_GUI gui;
        if (argc > 1 && juce::String(argv[1]) == "--verify") {
            require(argc == 3, "Usage: check_plugin_editor --verify <snapshot-directory>");
            verify(juce::File(argv[2]));
        } else {
            PlastiCZProcessor processor;
            if (argc > 2) processor.setClassicEditor(juce::String(argv[2]) != "modern");
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            dispatch();
            screenshot(*editor, argc > 1 ? juce::File(argv[1])
                : juce::File::getCurrentWorkingDirectory().getChildFile("editor-preview.png"));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
