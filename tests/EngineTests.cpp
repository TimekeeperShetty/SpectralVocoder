// Offline tests for VocoderEngine. No DAW, no audio device: feed known signals
// through the engine in host-sized blocks and check the output numerically.

#include "VocoderEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
    using Signal = std::vector<float>;
    constexpr float kTwoPi = juce::MathConstants<float>::twoPi;
    constexpr int   L      = VocoderEngine::getLatencySamples();

    int failures = 0;

    void check (bool ok, const char* name, const juce::String& detail)
    {
        std::printf ("  [%s] %s  (%s)\n", ok ? "PASS" : "FAIL", name, detail.toRawUTF8());
        if (! ok) ++failures;
    }

    /** Run the engine the way a host would: in blocks of blockSize samples. */
    Signal run (VocoderEngine& e, const Signal& voice, const Signal& carrier, int blockSize,
                VocoderEngine::Params p, int freezeOnAt = -1, int freezeOffAt = -1)
    {
        Signal out (voice.size(), 0.0f);
        const int total = (int) voice.size();

        for (int start = 0; start < total; start += blockSize)
        {
            const int n = std::min (blockSize, total - start);
            if (freezeOnAt  >= 0 && start >= freezeOnAt)  p.freezeActive = true;
            if (freezeOffAt >= 0 && start >= freezeOffAt) p.freezeActive = false;
            e.process (voice.data() + start, carrier.data() + start, out.data() + start, n, p);
        }
        return out;
    }

    Signal noise (int n, float amp, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> d (-amp, amp);
        Signal s ((size_t) n);
        for (auto& x : s) x = d (rng);
        return s;
    }

    /** Band-limited sawtooth (harmonics up to 0.45 * fs), like a synth would give. */
    Signal saw (int n, double fs, double hz, float amp)
    {
        Signal s ((size_t) n, 0.0f);
        for (int k = 1; hz * k < 0.45 * fs; ++k)
        {
            const double a = amp * 2.0 / (juce::MathConstants<double>::pi * k) * ((k % 2) ? 1.0 : -1.0);
            const double w = kTwoPi * hz * k / fs;
            for (int i = 0; i < n; ++i) s[(size_t) i] += (float) (a * std::sin (w * i));
        }
        return s;
    }

    /** A stand-in for speech: 140 Hz harmonics shaped by three formants, gated
        into "syllables" by a 3 Hz envelope. Returns the gate envelope too. */
    Signal fakeVoice (int n, double fs, Signal& gate)
    {
        Signal s ((size_t) n, 0.0f);
        gate.assign ((size_t) n, 0.0f);
        const double f0 = 140.0;
        const double formants[] = { 700.0, 1200.0, 2600.0 };

        for (int h = 1; f0 * h < 5000.0; ++h)
        {
            const double f = f0 * h;
            double a = 0.0;
            for (double fm : formants) a += std::exp (-std::pow ((f - fm) / 150.0, 2.0));
            for (int i = 0; i < n; ++i)
                s[(size_t) i] += (float) (0.15 * a * std::sin (kTwoPi * f * i / fs));
        }
        for (int i = 0; i < n; ++i)
        {
            const float g = (float) std::max (0.0, std::sin (kTwoPi * 3.0 * i / fs));
            gate[(size_t) i] = g;
            s[(size_t) i] *= g;
        }
        return s;
    }

    float rms (const Signal& s, int from, int to)
    {
        double acc = 0.0;
        for (int i = from; i < to; ++i) acc += (double) s[(size_t) i] * s[(size_t) i];
        return (float) std::sqrt (acc / std::max (1, to - from));
    }

    float peak (const Signal& s)
    {
        float m = 0.0f;
        for (float x : s) m = std::max (m, std::abs (x));
        return m;
    }

    bool allFinite (const Signal& s)
    {
        for (float x : s) if (! std::isfinite (x)) return false;
        return true;
    }

    float db (float x) { return juce::Decibels::gainToDecibels (x, -200.0f); }

    /** Energy at one frequency (Goertzel). */
    double toneEnergy (const Signal& s, int from, int to, double fs, double hz)
    {
        const double w = 2.0 * std::cos (2.0 * juce::MathConstants<double>::pi * hz / fs);
        double s1 = 0.0, s2 = 0.0;
        for (int i = from; i < to; ++i) { const double s0 = s[(size_t) i] + w * s1 - s2; s2 = s1; s1 = s0; }
        return s1 * s1 + s2 * s2 - w * s1 * s2;
    }

    double correlation (const std::vector<double>& a, const std::vector<double>& b)
    {
        const size_t n = std::min (a.size(), b.size());
        double ma = 0, mb = 0;
        for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= (double) n; mb /= (double) n;
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < n; ++i)
        {
            sab += (a[i] - ma) * (b[i] - mb);
            saa += (a[i] - ma) * (a[i] - ma);
            sbb += (b[i] - mb) * (b[i] - mb);
        }
        return sab / std::sqrt (saa * sbb + 1e-30);
    }
}

//==============================================================================
static void testPassthrough (double fs)
{
    std::printf ("\nSTFT passthrough at %.0f Hz (output should equal input delayed by %d samples)\n", fs, L);
    const int n = (int) fs * 3;
    const auto voice   = noise (n, 0.5f, 1);
    const auto carrier = noise (n, 0.5f, 2);

    for (int block : { 1, 37, 64, 441, 512, 1000, 4096 })
    {
        VocoderEngine e;
        e.prepare (fs);
        VocoderEngine::Params p;
        p.passthrough = true;
        const auto out = run (e, voice, carrier, block, p);

        float maxErr = 0.0f;
        for (int i = L; i < n; ++i)
            maxErr = std::max (maxErr, std::abs (out[(size_t) i] - voice[(size_t) (i - L)]));
        float pre = 0.0f;
        for (int i = 0; i < L; ++i) pre = std::max (pre, std::abs (out[(size_t) i]));

        check (maxErr < 1.0e-4f && pre < 1.0e-6f,
               ("block size " + juce::String (block)).toRawUTF8(),
               "max error " + juce::String (db (maxErr), 1) + " dB re input");
    }
}

static void testLatency()
{
    std::printf ("\nLatency\n");
    const double fs = 48000.0;
    const int n = 20000, at = 5000;
    Signal voice ((size_t) n, 0.0f), carrier ((size_t) n, 0.0f);
    voice[(size_t) at] = 1.0f;

    VocoderEngine e;
    e.prepare (fs);
    VocoderEngine::Params p;
    p.passthrough = true;
    const auto out = run (e, voice, carrier, 256, p);

    int argmax = 0;
    for (int i = 0; i < n; ++i) if (std::abs (out[(size_t) i]) > std::abs (out[(size_t) argmax])) argmax = i;

    check (argmax - at == L, "impulse comes out exactly getLatencySamples() later",
           "measured " + juce::String (argmax - at) + " samples, reported " + juce::String (L));
}

static void testModes (double fs)
{
    std::printf ("\nThe three modes at %.0f Hz (voice: 140 Hz formant buzz, gated at 3 Hz; carrier: 110 Hz saw)\n", fs);
    const int n = (int) fs * 4;
    Signal gate;
    const auto voice   = fakeVoice (n, fs, gate);
    const auto carrier = saw (n, fs, 110.0, 0.5f);
    const Signal silence ((size_t) n, 0.0f);

    std::printf ("  input levels: voice peak %.1f dB, carrier peak %.1f dB\n", db (peak (voice)), db (peak (carrier)));

    struct Mode { const char* name; float v, c; };
    for (const auto& m : { Mode { "vocoder", 1, 0 }, Mode { "cross-synthesis", 0, 1 }, Mode { "50/50 blend", 1, 1 } })
    {
        VocoderEngine e;
        e.prepare (fs);
        VocoderEngine::Params p;
        p.wVoc = m.v; p.wCross = m.c;
        const auto out = run (e, voice, carrier, 512, p);

        // Does the output follow the voice's syllables? Compare per-hop output level
        // with the (latency-shifted) voice gate.
        std::vector<double> outLevel, gateLevel;
        for (int i = L + 4096; i + 512 <= n; i += 512)
        {
            outLevel.push_back (rms (out, i, i + 512));
            double g = 0.0;
            for (int j = i - L; j < i - L + 512; ++j) g += gate[(size_t) j];
            gateLevel.push_back (g / 512.0);
        }
        const double r = correlation (outLevel, gateLevel);

        check (allFinite (out) && peak (out) > 1.0e-3f && r > 0.8, m.name,
               "output peak " + juce::String (db (peak (out)), 1) + " dB, rms "
                   + juce::String (db (rms (out, L, n)), 1) + " dB, follows voice r = " + juce::String (r, 2));
    }

    {
        VocoderEngine e;
        e.prepare (fs);
        VocoderEngine::Params p;
        const auto out = run (e, voice, silence, 512, p);
        check (peak (out) < 1.0e-6f, "vocoder is silent when the carrier is silent",
               "peak " + juce::String (db (peak (out)), 1) + " dB");
    }
    {
        VocoderEngine e;
        e.prepare (fs);
        VocoderEngine::Params p;
        const auto out = run (e, silence, carrier, 512, p);
        check (peak (out) < 1.0e-6f, "vocoder is silent when the voice is silent",
               "peak " + juce::String (db (peak (out)), 1) + " dB");
    }
}

static void testFreeze()
{
    std::printf ("\nFreeze (capture a 110 Hz saw at 1 s, then the carrier stops for 60 s)\n");
    const double fs = 48000.0;
    const int n = (int) fs * 62;
    auto carrier = saw (n, fs, 110.0, 0.5f);
    for (int i = (int) fs * 2; i < n; ++i) carrier[(size_t) i] = 0.0f;   // carrier stops at 2 s
    const Signal voice ((size_t) n, 0.0f);

    VocoderEngine e;
    e.prepare (fs);
    VocoderEngine::Params p;
    p.wVoc = 0.0f; p.wCross = 0.0f; p.wFreeze = 1.0f;
    const auto out = run (e, voice, carrier, 480, p, (int) fs * 1);

    const int sec = (int) fs;
    const float early = rms (out, 5 * sec, 6 * sec);
    const float late  = rms (out, 60 * sec, 61 * sec);
    check (allFinite (out) && early > 1.0e-3f && std::abs (db (late) - db (early)) < 0.5f,
           "drone holds steady for a minute after the carrier stops",
           "level at 5 s " + juce::String (db (early), 1) + " dB, at 60 s " + juce::String (db (late), 1) + " dB");

    const double on  = toneEnergy (out, 60 * sec, 61 * sec, fs, 110.0);
    const double off = toneEnergy (out, 60 * sec, 61 * sec, fs, 103.0);
    check (on > 100.0 * off, "frozen drone is still at 110 Hz after a minute",
           "110 Hz vs 103 Hz: " + juce::String (10.0 * std::log10 (on / (off + 1e-30)), 1) + " dB");

    VocoderEngine e2;
    e2.prepare (fs);
    const auto released = run (e2, voice, carrier, 480, p, (int) fs * 1, (int) fs * 10);
    check (peak (Signal (released.begin() + 11 * sec, released.end())) < 1.0e-6f,
           "releasing freeze returns to silence (voice and carrier both silent)",
           "peak after release " + juce::String (db (peak (Signal (released.begin() + 11 * sec, released.end()))), 1) + " dB");
}

static void testSpeed()
{
    std::printf ("\nCPU\n");
    const double fs = 48000.0;
    const int n = (int) fs * 20;
    Signal gate;
    const auto voice = fakeVoice (n, fs, gate);
    const auto carrier = saw (n, fs, 110.0, 0.5f);

    VocoderEngine e;
    e.prepare (fs);
    VocoderEngine::Params p;
    p.wVoc = 1; p.wCross = 1; p.wFreeze = 1;

    const auto t0 = std::chrono::steady_clock::now();
    const auto out = run (e, voice, carrier, 128, p, (int) fs * 5);
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

    check (allFinite (out) && secs < 20.0 * 0.25, "all three modes plus freeze, 128-sample blocks",
           juce::String (secs / 20.0 * 100.0, 2) + "% of one core (20 s of audio in " + juce::String (secs * 1000.0, 0) + " ms)");
}

//==============================================================================
int main()
{
    std::printf ("VocoderEngine tests  (fftSize %d, hop %d, %d mel bands)\n",
                 VocoderEngine::fftSize, VocoderEngine::hopSize, VocoderEngine::numBands);

    for (double fs : { 44100.0, 48000.0, 96000.0 })
        testPassthrough (fs);
    testLatency();
    for (double fs : { 44100.0, 48000.0 })
        testModes (fs);
    testFreeze();
    testSpeed();

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
