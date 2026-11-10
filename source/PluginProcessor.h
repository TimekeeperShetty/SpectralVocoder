#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "VocoderEngine.h"

/**
    The plugin itself, as the DAW sees it.

    Inputs:  bus 0 "Modulator" = the track the plugin sits on (your voice)
             bus 1 "Carrier"   = the sidechain input (a synth, pad, noise...)
    Output:  bus 0 "Output"

    It owns the parameters (APVTS) and the VocoderEngine. It does no DSP itself:
    each block it mixes both inputs to mono, hands them to the engine with a
    snapshot of the parameters, and copies the result to every output channel.
*/
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override = default;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==============================================================================
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override                                 { return 1; }
    int getCurrentProgram() override                              { return 0; }
    void setCurrentProgram (int) override                         {}
    const juce::String getProgramName (int) override              { return {}; }
    void changeProgramName (int, const juce::String&) override    {}

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    /** All user-facing parameters. The editor attaches its controls to these. */
    juce::AudioProcessorValueTreeState apvts;

    /** Parameter IDs, in one place so the editor and the DAW agree on them.
        Changing an ID after release breaks saved sessions, so treat these as fixed. */
    struct IDs
    {
        static constexpr const char* wVoc        = "wVoc";
        static constexpr const char* wCross      = "wCross";
        static constexpr const char* wFreeze     = "wFreeze";
        static constexpr const char* release     = "release";
        static constexpr const char* minBandHz   = "minBandHz";
        static constexpr const char* freeze      = "freeze";
        static constexpr const char* gainDb      = "gainDb";
        static constexpr const char* passthrough = "passthrough";
    };

    /** Input peak levels from the last block, for the editor's readout.
        Written on the audio thread, read on the UI thread. */
    std::atomic<float> voicePeak   { 0.0f };
    std::atomic<float> carrierPeak { 0.0f };
    std::atomic<bool>  carrierConnected { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    VocoderEngine engine;

    // Mono work buffers, sized in prepareToPlay so processBlock never allocates.
    juce::AudioBuffer<float> monoVoice, monoCarrier, monoOut;

    // Lock-free pointers to the current parameter values, fetched once.
    std::atomic<float>* pVoc         = nullptr;
    std::atomic<float>* pCross       = nullptr;
    std::atomic<float>* pFreezeW     = nullptr;
    std::atomic<float>* pRelease     = nullptr;
    std::atomic<float>* pMinBand     = nullptr;
    std::atomic<float>* pFreezeOn    = nullptr;
    std::atomic<float>* pGainDb      = nullptr;
    std::atomic<float>* pPassthrough = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
