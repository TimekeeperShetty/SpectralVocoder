# Spectral Vocoder

A real-time JUCE plugin port of the Python `vocoder_blend` notebook. It blends a channel vocoder, cross-synthesis and spectral freeze. The plugin's own track is the voice (modulator), and the DAW's sidechain input is the carrier.

Formats: VST3, AU (macOS) and Standalone. JUCE 8.0.14.

## Build

New to the project? Start with [CONTRIBUTING.md](CONTRIBUTING.md). It covers what to install on macOS and Windows, and how we work with git.

JUCE is a git submodule, so clone with:

```bash
git clone --recurse-submodules <repo-url>
```

Then build with the preset for your machine:

```bash
# macOS
cmake --preset mac-release
cmake --build --preset mac-release
ctest --preset mac-release

# Windows
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release
```

The first configure takes about a minute because JUCE builds its own helper tool. In VS Code, the CMake Tools extension picks up the same presets.

On macOS, the plugins are copied automatically after a successful build to:

- `~/Library/Audio/Plug-Ins/VST3/Spectral Vocoder.vst3`
- `~/Library/Audio/Plug-Ins/Components/Spectral Vocoder.component` (AU)

On Windows, copying into `C:\Program Files\Common Files\VST3` needs admin rights, so it is off. Copy `build\windows\SpectralVocoder_artefacts\Release\VST3\Spectral Vocoder.vst3` there yourself, or configure with `-DSV_COPY_PLUGINS=ON` from an admin terminal.

The Standalone app is at `build/<preset>/SpectralVocoder_artefacts/Release/Standalone/`.

If CMake behaves strangely, delete the `build` folder and configure again. Nothing of ours lives in there.

## Source files

| File | What it does |
|---|---|
| `source/VocoderEngine.h/.cpp` | The DSP: the notebook's per-frame math, rebuilt for streaming. Collects samples into 512-sample hops, runs a 2048-point STFT frame, and overlap-adds the result into an output FIFO. No JUCE plugin code. |
| `source/PluginProcessor.h/.cpp` | The plugin as the DAW sees it: buses (Modulator in, Carrier sidechain in, Output), parameters (APVTS), state save/restore. Mixes both inputs to mono and calls the engine every block. |
| `source/PluginEditor.h/.cpp` | Temporary UI: one slider or toggle per parameter, plus input level readouts. |
| `tests/EngineTests.cpp` | Offline tests for the engine. No DAW needed. |

## Parameters

These IDs are saved in DAW sessions. Don't rename them once people are using the plugin.

| ID | Name | Range | Default |
|---|---|---|---|
| `wVoc` | Vocoder | 0–1 | 1.0 |
| `wCross` | Cross-Synthesis | 0–1 | 0.0 |
| `wFreeze` | Freeze Mix | 0–1 | 0.5 |
| `freeze` | Freeze | on/off | off |
| `gainDb` | Output Gain | −36 to +36 dB | 0 dB |
| `passthrough` | STFT Passthrough Test | on/off | off |

The three weights are normalised to sum to 1. Freeze Mix only counts while Freeze is on.

## Testing in Ableton

1. Put Spectral Vocoder on the voice track.
2. Under the device's **Sidechain** section, choose the carrier track (a synth pad or saw works well).
3. The readout at the bottom of the plugin shows both input levels. If the carrier shows "silent", the sidechain isn't routed.
4. With no sidechain, the output is silent. That's expected: the vocoder needs a carrier. Turn on **STFT passthrough test** to hear the voice through the full STFT path instead.

## Engine tests

The presets build the tests, and `ctest --preset <preset>` runs them (see Build above). To see the full report, run the test program directly:

```bash
./build/mac-release/EngineTests_artefacts/Release/EngineTests
```

CI runs these tests on macOS and Windows for every pull request.

The tests check:

- Passthrough reconstruction at 44.1, 48 and 96 kHz and block sizes from 1 to 4096.
- That latency is exactly 2048 samples.
- That each mode follows the voice.
- Silence in, silence out.
- That freeze stays steady for a minute.
- CPU use.

## Notes on the port

These are the places where the plugin differs from the notebook:

- **No final peak-normalise.** A streaming plugin can't see the whole output. Level now comes from the overlap-add gain (÷1.5), a fixed vocoder scale, and a `tanh` soft clip after Output Gain.
- **Vocoder level matched to cross-synthesis.** The vocoder path multiplies two FFT magnitudes, so unscaled it is about 37 dB louder and drowns out the other modes in a blend. A fixed scale (measured across several voices and carriers) brings the two to within about ±5 dB of each other.
- **Freeze is a live button.** Pressing it captures the current carrier frame. Releasing it lets the next press capture a fresh one.
- **Freeze uses phase locking.** Each bin follows the frequency of the harmonic peak it belongs to. Without this, low carriers (whose harmonics are only a few bins apart) swell and fade by several dB while frozen.
- **Periodic Hann window** (not `np.hanning`, which is symmetric), so overlap-add reconstructs exactly.
- **Inputs are mixed to mono**, as in the notebook, and the output is copied to all channels.
- **Latency is 2048 samples** (about 43 ms at 48 kHz), reported to the host for delay compensation.
