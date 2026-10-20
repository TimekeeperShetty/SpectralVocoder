#include "VocoderEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float kPi    = juce::MathConstants<float>::pi;
    constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

    /** Wrap a phase into [-pi, pi). */
    inline float wrapPhase (float x) noexcept
    {
        return x - kTwoPi * std::floor ((x + kPi) / kTwoPi);
    }

    inline float hzToMel (float hz)  noexcept { return 2595.0f * std::log10 (1.0f + hz / 700.0f); }
    inline float melToHz (float mel) noexcept { return 700.0f * (std::pow (10.0f, mel / 2595.0f) - 1.0f); }

    /** Hann applied twice (analysis and synthesis) at 75% overlap sums to a
        constant 1.5. Dividing by it gives unity gain. The notebook got away
        without this because it peak-normalised the whole output at the end,
        which a streaming plugin can't do. */
    constexpr float kOlaGain = 1.0f / 1.5f;

    /** The channel-vocoder path multiplies two FFT magnitudes (voice envelope x
        carrier), while cross-synthesis uses only one. Unscaled, the vocoder path is
        tens of dB louder, so in a blend it drowns out the other modes (the
        notebook's final peak-normalise hid this). This fixed scale brings the
        vocoder to roughly the same level as cross-synthesis. It was measured with
        buzzy and whispered voices against saw, chord and noise carriers (spread
        about +-4 dB). It changes level only, not the sound, and keeps the
        carrier's own dynamics. */
    constexpr float kVocScale = 0.15f;
}

//==============================================================================
void VocoderEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    // Periodic Hann (divides by N, not N-1 like np.hanning). Periodic is the
    // version that sums to an exact constant under overlap-add.
    window.resize (fftSize);
    for (int n = 0; n < fftSize; ++n)
        window[(size_t) n] = 0.5f - 0.5f * std::cos (kTwoPi * (float) n / (float) fftSize);

    // Mel-spaced band edges from 0 Hz to Nyquist, as FFT bin indices.
    bandEdges.resize (numBands + 1);
    const float melHi = hzToMel ((float) sampleRate * 0.5f);
    for (int b = 0; b <= numBands; ++b)
    {
        const float hz = melToHz (melHi * (float) b / (float) numBands);
        const int bin  = (int) std::lround (hz * (float) fftSize / (float) sampleRate);
        bandEdges[(size_t) b] = juce::jlimit (0, numBins, bin);
    }
    bandEdges[(size_t) numBands] = numBins;   // make sure the Nyquist bin is included

    // Phase a bin's centre frequency advances by in one hop, 2*pi*k*hop/N, kept
    // modulo 2*pi. Unwrapped it reaches ~1600 rad for high bins, where a float
    // only resolves ~1e-4 rad. During a long freeze that error accumulates
    // every frame until neighbouring bins drift apart and the drone fades.
    omega.resize (numBins);
    for (int k = 0; k < numBins; ++k)
        omega[(size_t) k] = wrapPhase (kTwoPi * (float) ((k * hopSize) % fftSize) / (float) fftSize);

    stageV.assign (hopSize, 0.0f);
    stageC.assign (hopSize, 0.0f);
    frameV.assign (fftSize, 0.0f);
    frameC.assign (fftSize, 0.0f);

    fftV  .assign (2 * fftSize, 0.0f);
    fftC  .assign (2 * fftSize, 0.0f);
    fftOut.assign (2 * fftSize, 0.0f);

    for (auto* v : { &magV, &magC, &phaseC, &lastPhaseC, &trueAdvC, &envelope,
                     &freezeMag, &freezePhase, &freezeOmega })
        v->assign (numBins, 0.0f);

    peakBins.assign (numBins, 0);

    accum  .assign (fftSize, 0.0f);
    outFifo.assign (fftSize, 0.0f);

    reset();
}

void VocoderEngine::reset()
{
    for (auto* v : { &stageV, &stageC, &frameV, &frameC, &lastPhaseC, &accum, &outFifo })
        std::fill (v->begin(), v->end(), 0.0f);

    stageCount = 0;
    outRead = outWrite = outCount = 0;
    freezeCaptured = false;
}

//==============================================================================
void VocoderEngine::process (const float* voiceIn,
                             const float* carrierIn,
                             float* out,
                             int numSamples,
                             const Params& params) noexcept
{
    const int fifoSize = (int) outFifo.size();

    for (int i = 0; i < numSamples; ++i)
    {
        // Read the output before (possibly) running a frame. This ordering makes
        // the latency exactly fftSize samples.
        if (outCount > 0)
        {
            out[i] = outFifo[(size_t) outRead];
            outRead = (outRead + 1) % fifoSize;
            --outCount;
        }
        else
        {
            out[i] = 0.0f;   // only during the first hop after a reset
        }

        stageV[(size_t) stageCount] = voiceIn[i];
        stageC[(size_t) stageCount] = carrierIn[i];

        if (++stageCount == hopSize)
        {
            // Slide both analysis frames left by one hop and append the new hop.
            // This replaces the notebook's `for start in range(0, len(x) - n_fft, hop)`.
            constexpr size_t keep = (size_t) (fftSize - hopSize);
            std::memmove (frameV.data(), frameV.data() + hopSize, keep * sizeof (float));
            std::memmove (frameC.data(), frameC.data() + hopSize, keep * sizeof (float));
            std::copy (stageV.begin(), stageV.end(), frameV.begin() + (long) keep);
            std::copy (stageC.begin(), stageC.end(), frameC.begin() + (long) keep);

            stageCount = 0;
            processFrame (params);
        }
    }
}

//==============================================================================
/** Phase locking for the frozen frame. This is a change from the notebook.

    Each harmonic of the carrier spreads over several bins (the Hann window's main
    lobe is 4 bins wide). Bins near the peak measure the harmonic's frequency
    correctly, but bins between two close harmonics (low notes, where harmonics
    are only ~5 bins apart) measure a mixture of both. Left alone they rotate at
    the wrong speed and slowly drift against their neighbours, so the frozen
    drone swells and fades by several dB.

    The fix: give every bin the frequency of the spectral peak it belongs to, so
    each harmonic's bins rotate together and the frozen frame repeats exactly.
    Runs once per freeze press, allocates nothing. */
void VocoderEngine::lockFreezeToPeaks() noexcept
{
    int numPeaks = 0;
    for (int k = 1; k < numBins - 1; ++k)
    {
        const float m = freezeMag[(size_t) k];
        if (m > 1.0e-9f && m > freezeMag[(size_t) k - 1] && m >= freezeMag[(size_t) k + 1])
            peakBins[(size_t) numPeaks++] = k;
    }

    if (numPeaks == 0)
        return;

    // Each peak owns the bins from the trough before it to the trough after it.
    int regionStart = 0;
    for (int i = 0; i < numPeaks; ++i)
    {
        const int peak = peakBins[(size_t) i];
        int regionEnd  = numBins;

        if (i + 1 < numPeaks)
        {
            const int next = peakBins[(size_t) i + 1];
            regionEnd = peak + 1;
            for (int k = peak + 1; k < next; ++k)
                if (freezeMag[(size_t) k] < freezeMag[(size_t) regionEnd])
                    regionEnd = k;
        }

        for (int k = regionStart; k < regionEnd; ++k)
            freezeOmega[(size_t) k] = trueAdvC[(size_t) peak];

        regionStart = regionEnd;
    }
}

//==============================================================================
void VocoderEngine::processFrame (const Params& params) noexcept
{
    // ---- 1. window and FFT both signals ----
    for (int n = 0; n < fftSize; ++n)
    {
        fftV[(size_t) n] = frameV[(size_t) n] * window[(size_t) n];
        fftC[(size_t) n] = frameC[(size_t) n] * window[(size_t) n];
    }
    std::fill (fftV.begin() + fftSize, fftV.end(), 0.0f);
    std::fill (fftC.begin() + fftSize, fftC.end(), 0.0f);

    fft.performRealOnlyForwardTransform (fftV.data(), true);
    fft.performRealOnlyForwardTransform (fftC.data(), true);

    // JUCE stores the result as interleaved (re, im) pairs, which has the same
    // memory layout as std::complex<float>.
    auto* V = reinterpret_cast<std::complex<float>*> (fftV.data());
    auto* C = reinterpret_cast<std::complex<float>*> (fftC.data());
    auto* Z = reinterpret_cast<std::complex<float>*> (fftOut.data());

    if (params.passthrough)
    {
        // Test mode: Z = V. If the output equals the input (delayed by the
        // latency) at the same level, windowing, hop bookkeeping, overlap-add
        // gain and the FIFO are all correct.
        std::copy (V, V + numBins, Z);
    }
    else
    {
        // ---- 2. magnitudes, carrier phase, carrier true phase advance ----
        // The carrier's true advance is tracked every frame so a freeze can be
        // captured whenever the user presses the button. (The notebook also
        // computed the voice's, but never used it.)
        for (int k = 0; k < numBins; ++k)
        {
            magV[(size_t) k]   = std::abs (V[k]);
            magC[(size_t) k]   = std::abs (C[k]);
            phaseC[(size_t) k] = std::arg (C[k]);

            const float deviation = phaseC[(size_t) k] - lastPhaseC[(size_t) k] - omega[(size_t) k];
            trueAdvC[(size_t) k]  = wrapPhase (omega[(size_t) k] + wrapPhase (deviation));
            lastPhaseC[(size_t) k] = phaseC[(size_t) k];
        }

        // ---- 3. voice envelope: average magnitude within each mel band ----
        for (int b = 0; b < numBands; ++b)
        {
            const int lo = bandEdges[(size_t) b];
            const int hi = bandEdges[(size_t) b + 1];
            if (hi <= lo)
                continue;   // empty low band at small FFT sizes; the next band starts here

            float sum = 0.0f;
            for (int k = lo; k < hi; ++k)
                sum += magV[(size_t) k];

            const float mean = sum / (float) (hi - lo);
            for (int k = lo; k < hi; ++k)
                envelope[(size_t) k] = mean;
        }

        // ---- 4. freeze: capture on the rising edge, release on the falling edge ----
        if (params.freezeActive && ! freezeCaptured)
        {
            std::copy (magC.begin(),     magC.end(),     freezeMag.begin());
            std::copy (phaseC.begin(),   phaseC.end(),   freezePhase.begin());
            std::copy (trueAdvC.begin(), trueAdvC.end(), freezeOmega.begin());
            lockFreezeToPeaks();
            freezeCaptured = true;
        }
        else if (! params.freezeActive)
        {
            freezeCaptured = false;
        }

        // ---- 5. normalise the weights so they sum to 1 ----
        // While nothing is frozen, the freeze weight is dropped so it doesn't
        // just make everything quieter.
        float wv = std::max (0.0f, params.wVoc);
        float wc = std::max (0.0f, params.wCross);
        float wf = freezeCaptured ? std::max (0.0f, params.wFreeze) : 0.0f;

        const float total = wv + wc + wf;
        if (total <= 1.0e-6f) { wv = 1.0f; wc = 0.0f; wf = 0.0f; }
        else                  { wv /= total; wc /= total; wf /= total; }

        // ---- 6. build the three spectra and mix them ----
        for (int k = 0; k < numBins; ++k)
        {
            const auto ck = C[k];
            const float mc = magC[(size_t) k];

            // Channel vocoder: |C| * env * e^{i*phaseC} is just env * C.
            const auto zVoc = (kVocScale * envelope[(size_t) k]) * ck;

            // Cross-synthesis: the voice's magnitude on the carrier's phase.
            const auto unitC  = mc > 1.0e-9f ? ck / mc : std::complex<float> (1.0f, 0.0f);
            const auto zCross = magV[(size_t) k] * unitC;

            std::complex<float> zFreeze (0.0f, 0.0f);
            if (wf > 0.0f)
            {
                const float ph = freezePhase[(size_t) k];
                zFreeze = std::polar (freezeMag[(size_t) k], ph);
            }

            Z[k] = wv * zVoc + wc * zCross + wf * zFreeze;
        }

        // Advance each frozen bin by its true frequency. Keeping a wrapped running
        // phase (rather than phase0 + n * omega) avoids float precision loss when
        // freeze is held for minutes.
        if (freezeCaptured)
            for (int k = 0; k < numBins; ++k)
                freezePhase[(size_t) k] = wrapPhase (freezePhase[(size_t) k] + freezeOmega[(size_t) k]);
    }

    // DC and Nyquist must be real for a real-valued output.
    Z[0]           = { Z[0].real(), 0.0f };
    Z[fftSize / 2] = { Z[fftSize / 2].real(), 0.0f };

    // ---- 7. inverse FFT (JUCE only reads the first numBins values and scales by 1/N) ----
    fft.performRealOnlyInverseTransform (fftOut.data());

    // ---- 8. synthesis window and overlap-add ----
    for (int n = 0; n < fftSize; ++n)
        accum[(size_t) n] += fftOut[(size_t) n] * window[(size_t) n] * kOlaGain;

    // The first hop of the accumulator has now had all four overlapping frames
    // added to it, so it is finished: move it to the output FIFO.
    const int fifoSize = (int) outFifo.size();
    for (int n = 0; n < hopSize; ++n)
    {
        float y = accum[(size_t) n] * params.outputGain;
        if (! params.passthrough)
            y = std::tanh (y);   // soft clip: protects ears and speakers while levels are untuned

        outFifo[(size_t) outWrite] = y;
        outWrite = (outWrite + 1) % fifoSize;
        ++outCount;
    }

    std::memmove (accum.data(), accum.data() + hopSize, (size_t) (fftSize - hopSize) * sizeof (float));
    std::fill (accum.end() - hopSize, accum.end(), 0.0f);
}
