#pragma once

#include "PluginProcessor.h"

/**
    Temporary editor: one control per parameter plus an input-level readout,
    so the DSP can be tested in a DAW. The team builds the real UI on top of this.

    Each control is tied to its parameter with an APVTS attachment. The attachment
    keeps the control and the parameter in sync both ways (moving the slider sets
    the parameter, and DAW automation moves the slider), so the editor never
    touches the audio thread directly.
*/
class AudioPluginAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                              private juce::Timer
{
public:
    explicit AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    /** A labelled horizontal slider bound to one parameter. */
    struct LabelledSlider
    {
        juce::Label  label;
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        std::unique_ptr<SliderAttachment> attachment;
    };

    void setUpSlider (LabelledSlider&, const juce::String& paramID, const juce::String& name);

    AudioPluginAudioProcessor& processorRef;

    LabelledSlider vocoder, cross, freezeMix, gain;

    juce::ToggleButton freezeButton      { "Freeze" };
    juce::ToggleButton passthroughButton { "STFT passthrough test (output = voice)" };
    std::unique_ptr<ButtonAttachment> freezeAttachment, passthroughAttachment;

    juce::Label levelReadout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
