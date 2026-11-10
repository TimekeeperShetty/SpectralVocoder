#include "PluginEditor.h"

using IDs = AudioPluginAudioProcessor::IDs;

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setUpSlider (vocoder,   IDs::wVoc,    "Vocoder");
    setUpSlider (cross,     IDs::wCross,  "Cross-Synthesis");
    setUpSlider (freezeMix, IDs::wFreeze, "Freeze Mix");
    setUpSlider (release,   IDs::release, "Release");
    setUpSlider (minBand,   IDs::minBandHz, "Min Band Width");
    setUpSlider (gain,      IDs::gainDb,  "Output Gain");

    addAndMakeVisible (freezeButton);
    addAndMakeVisible (passthroughButton);
    freezeAttachment      = std::make_unique<ButtonAttachment> (processorRef.apvts, IDs::freeze,      freezeButton);
    passthroughAttachment = std::make_unique<ButtonAttachment> (processorRef.apvts, IDs::passthrough, passthroughButton);

    levelReadout.setJustificationType (juce::Justification::centredLeft);
    levelReadout.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (levelReadout);

    setResizable (true, true);
    setResizeLimits (720, 372, 1400, 680);
    setSize (720, 372);
    startTimerHz (10);
}

void AudioPluginAudioProcessorEditor::setUpSlider (LabelledSlider& s, const juce::String& paramID,
                                                   const juce::String& name)
{
    s.label.setText (name, juce::dontSendNotification);
    s.label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (s.label);

    s.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    addAndMakeVisible (s.slider);

    // Create the attachment after the slider exists; it sets the range and value.
    s.attachment = std::make_unique<SliderAttachment> (processorRef.apvts, paramID, s.slider);
}

//==============================================================================
void AudioPluginAudioProcessorEditor::timerCallback()
{
    auto toDb = [] (float peak)
    {
        return peak > 1.0e-5f ? juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dB"
                              : juce::String ("silent");
    };

    const auto carrierText = processorRef.carrierConnected.load()
                                 ? toDb (processorRef.carrierPeak.load())
                                 : juce::String ("no sidechain bus");

    levelReadout.setText ("Voice in: " + toDb (processorRef.voicePeak.load())
                              + "     Carrier (sidechain): " + carrierText,
                          juce::dontSendNotification);
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Spectral Vocoder", getLocalBounds().removeFromTop (36).reduced (12, 0),
                juce::Justification::centredLeft);
}

void AudioPluginAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (28);   // title, drawn in paint()

    for (auto* s : { &vocoder, &cross, &freezeMix, &release, &minBand, &gain })
    {
        auto row = area.removeFromTop (32);
        s->label.setBounds (row.removeFromLeft (120));
        s->slider.setBounds (row);
        area.removeFromTop (4);
    }

    area.removeFromTop (6);
    freezeButton.setBounds (area.removeFromTop (26));
    passthroughButton.setBounds (area.removeFromTop (26));
    area.removeFromTop (6);
    levelReadout.setBounds (area.removeFromTop (24));
}
