#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

/**
    Real-time port of the notebook's vocoder_blend().

    The notebook processes a whole file at once. Here, audio arrives in blocks of
    whatever size the host chooses (64, 128, 441...). The engine collects samples
    until it has one hop (512), slides its 2048-sample analysis window along, and
    runs one STFT frame. Output comes out of a FIFO, so the plugin has a fixed
    latency of exactly fftSize samples, which the processor reports to the host.

    Per frame it computes all three modes and mixes them with normalised weights:
      - channel vocoder: voice's mel-band envelope x carrier spectrum
      - cross-synthesis: voice's full magnitude on the carrier's phase
      - spectral freeze: a captured carrier frame, its phases advancing at their
                         measured true frequencies

    Threading: prepare() and reset() allocate and must be called from the message
    thread (prepareToPlay). process() never allocates or locks, so it is safe on
    the audio thread.
*/
class VocoderEngine
{
public:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize  = 1 << fftOrder;   // 2048 (notebook: n_fft)
    static constexpr int hopSize  = fftSize / 4;     // 512  (notebook: hop, 75% overlap)
    static constexpr int numBins  = fftSize / 2 + 1; // 1025 non-negative-frequency bins
    static constexpr int numBands = 24;              // mel bands for the voice envelope

    /** Everything the user controls, copied in fresh every block by the processor.
        The engine keeps no user-facing state of its own. */
    struct Params
    {
        float wVoc         = 1.0f;   // channel vocoder weight
        float wCross       = 0.0f;   // cross-synthesis weight
        float wFreeze      = 0.0f;   // freeze weight (only counts while frozen)
        bool  freezeActive = false;  // capture on rising edge, release on falling edge
        float outputGain   = 1.0f;   // linear gain before the soft clipper
        bool  passthrough  = false;  // test mode: output = voice, through the full STFT
    };

    void prepare (double sampleRate);
    void reset();

    /** voiceIn, carrierIn and out are mono buffers of numSamples.
        out must not alias either input. */
    void process (const float* voiceIn,
                  const float* carrierIn,
                  float* out,
                  int numSamples,
                  const Params& params) noexcept;

    /** Output sample n corresponds to input sample n - getLatencySamples(). */
    static constexpr int getLatencySamples() noexcept { return fftSize; }

private:
    void processFrame (const Params& params) noexcept;
    void lockFreezeToPeaks() noexcept;

    double sampleRate = 44100.0;
    juce::dsp::FFT fft { fftOrder };

    // ---- tables, built once in prepare() ----
    std::vector<float> window;     // periodic Hann, fftSize
    std::vector<int>   bandEdges;  // numBands + 1 bin indices, mel-spaced
    std::vector<float> omega;      // expected phase advance per hop, per bin

    // ---- input staging: collect one hop, then slide the analysis frames ----
    std::vector<float> stageV, stageC;
    int stageCount = 0;
    std::vector<float> frameV, frameC;   // most recent fftSize input samples

    // ---- FFT work buffers (JUCE's real-only FFT needs 2 * fftSize floats) ----
    std::vector<float> fftV, fftC, fftOut;

    // ---- per-frame spectra (Python locals become preallocated members) ----
    std::vector<float> magV, magC, phaseC, lastPhaseC, trueAdvC, envelope;

    // ---- freeze snapshot ----
    std::vector<float> freezeMag, freezePhase, freezeOmega;
    std::vector<int>   peakBins;   // scratch for lockFreezeToPeaks()
    bool freezeCaptured = false;

    // ---- overlap-add and output FIFO ----
    std::vector<float> accum;     // fftSize
    std::vector<float> outFifo;   // circular
    int outRead = 0, outWrite = 0, outCount = 0;

    JUCE_LEAK_DETECTOR (VocoderEngine)
};
