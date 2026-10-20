#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Modulator", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Carrier",   juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    pVoc         = apvts.getRawParameterValue (IDs::wVoc);
    pCross       = apvts.getRawParameterValue (IDs::wCross);
    pFreezeW     = apvts.getRawParameterValue (IDs::wFreeze);
    pFreezeOn    = apvts.getRawParameterValue (IDs::freeze);
    pGainDb      = apvts.getRawParameterValue (IDs::gainDb);
    pPassthrough = apvts.getRawParameterValue (IDs::passthrough);

    // The latency never changes, so report it straight away (some hosts ask
    // before calling prepareToPlay).
    setLatencySamples (VocoderEngine::getLatencySamples());
}

juce::AudioProcessorValueTreeState::ParameterLayout AudioPluginAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // How values are shown as text in the editor and in the DAW's automation lanes.
    const auto twoDecimals = AudioParameterFloatAttributes()
                                 .withStringFromValueFunction ([] (float v, int) { return String (v, 2); });
    const auto decibels = AudioParameterFloatAttributes()
                              .withLabel ("dB")
                              .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; });

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::wVoc, 1 }, "Vocoder",
                                                       NormalisableRange<float> (0.0f, 1.0f), 1.0f, twoDecimals));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::wCross, 1 }, "Cross-Synthesis",
                                                       NormalisableRange<float> (0.0f, 1.0f), 0.0f, twoDecimals));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::wFreeze, 1 }, "Freeze Mix",
                                                       NormalisableRange<float> (0.0f, 1.0f), 0.5f, twoDecimals));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { IDs::freeze, 1 }, "Freeze", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { IDs::gainDb, 1 }, "Output Gain",
                                                       NormalisableRange<float> (-36.0f, 36.0f, 0.1f), 0.0f, decibels));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { IDs::passthrough, 1 }, "STFT Passthrough Test", false));

    return layout;
}

//==============================================================================
bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn  = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    // Main in and out: mono or stereo, and the same as each other.
    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;
    if (mainIn != mainOut)
        return false;

    // Sidechain: the host may leave it disconnected, or offer mono or stereo.
    if (layouts.inputBuses.size() > 1)
    {
        const auto side = layouts.getChannelSet (true, 1);
        if (! side.isDisabled()
            && side != juce::AudioChannelSet::mono()
            && side != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);

    const int capacity = juce::jmax (samplesPerBlock, 512);
    monoVoice  .setSize (1, capacity, false, true, false);
    monoCarrier.setSize (1, capacity, false, true, false);
    monoOut    .setSize (1, capacity, false, true, false);

    setLatencySamples (VocoderEngine::getLatencySamples());
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    const double sr = getSampleRate();
    return sr > 0.0 ? (double) VocoderEngine::getLatencySamples() / sr : 0.0;
}

//==============================================================================
namespace
{
    /** Average all channels of src[start, start + n) into channel 0 of dest. */
    void mixToMono (const juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dest, int start, int n)
    {
        dest.clear (0, 0, n);
        const int numCh = src.getNumChannels();
        for (int ch = 0; ch < numCh; ++ch)
            dest.addFrom (0, 0, src, ch, start, n, 1.0f / (float) numCh);
    }
}

void AudioPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0)
        return;

    // The host packs every bus into one buffer. getBusBuffer gives a view of one bus.
    const auto modBus     = getBusBuffer (buffer, true, 0);
    const auto carrierBus = getBusBuffer (buffer, true, 1);   // 0 channels if no sidechain
    auto outBus           = getBusBuffer (buffer, false, 0);

    voicePeak.store   (modBus.getNumChannels()     > 0 ? modBus.getMagnitude (0, numSamples)     : 0.0f);
    carrierPeak.store (carrierBus.getNumChannels() > 0 ? carrierBus.getMagnitude (0, numSamples) : 0.0f);
    carrierConnected.store (carrierBus.getNumChannels() > 0);

    // Snapshot the parameters once per block.
    VocoderEngine::Params p;
    p.wVoc         = pVoc->load();
    p.wCross       = pCross->load();
    p.wFreeze      = pFreezeW->load();
    p.freezeActive = pFreezeOn->load() > 0.5f;
    p.outputGain   = juce::Decibels::decibelsToGain (pGainDb->load());
    p.passthrough  = pPassthrough->load() > 0.5f;

    // Hosts can occasionally send a bigger block than promised in prepareToPlay,
    // so work through the block in chunks that fit the preallocated buffers.
    const int capacity = monoOut.getNumSamples();

    for (int start = 0; start < numSamples; start += capacity)
    {
        const int n = juce::jmin (capacity, numSamples - start);

        mixToMono (modBus,     monoVoice,   start, n);
        mixToMono (carrierBus, monoCarrier, start, n);   // stays silent with no sidechain

        engine.process (monoVoice.getReadPointer (0),
                        monoCarrier.getReadPointer (0),
                        monoOut.getWritePointer (0),
                        n, p);

        // Write after reading the inputs: the output bus shares memory with the main input.
        for (int ch = 0; ch < outBus.getNumChannels(); ++ch)
            outBus.copyFrom (ch, start, monoOut, 0, 0, n);
    }
}

//==============================================================================
juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
