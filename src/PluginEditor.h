#pragma once
#include "PluginProcessor.h"
class PlastiCZEditor final : public juce::AudioProcessorEditor, private juce::ChangeListener {
public:
    explicit PlastiCZEditor(PlastiCZProcessor&);
    ~PlastiCZEditor() override;
    void resized() override;
private:
    struct Interface;
    void updateLayout();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    PlastiCZProcessor& processor;
    std::unique_ptr<Interface> interface;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlastiCZEditor)
};
