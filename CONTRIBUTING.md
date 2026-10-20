# How we work

This is our working agreement. If something here stops working for us, change it at a Tuesday meeting and update this file.

## Schedule

| Date | Milestone |
|---|---|
| Fri 10-23 | Everyone has built and run the plugin on their own machine |
| Fri 10-30 | Signal path works in a DAW with a sidechain carrier |
| Fri 11-13 | **Feature freeze.** After this, only fixes. |
| Fri 11-20 | Release candidate. Last day teammate work is accepted. |
| Tue 11-24 | **Deliver.** Nothing slips into December. |

- **Tuesday 6:30 pm meeting:** tasks are assigned as GitHub Issues.
- **Friday:** check in by updating your issues on the project board.
- A task that is running late gets help or gets cut. Deadlines don't move.

## One-time setup

### macOS

1. Install **Xcode** from the App Store and open it once to accept the licence.
2. Install CMake: `brew install cmake` (or from cmake.org).

### Windows

1. Install **Visual Studio 2022 or newer, Community edition**. In the installer, tick the **Desktop development with C++** workload. VS Code alone has no C++ compiler, so you need this even if you never open Visual Studio.
2. Install CMake from cmake.org, and tick "Add CMake to the PATH".
3. Install **Git for Windows**.

### Both

1. Install **VS Code**. When you open the project, accept the prompt to install the recommended extensions (CMake Tools and C/C++).
2. Clone with the JUCE submodule:

   ```bash
   git clone --recurse-submodules <repo-url>
   ```

   If you already cloned without it, run `git submodule update --init`.
3. In VS Code, run **CMake: Select Configure Preset**. Choose `mac-release` or `windows`, then **CMake: Build**. Or use the commands in the README.

**Your first task:** build the plugin, open the Standalone app, and load the VST3 in your DAW. Post a screenshot on your onboarding issue. If something fails, post the error there.

## Git workflow

- `main` must always build. Never commit to it directly.
- For each task, create a branch from an up-to-date `main`. Name it `<area>/<short-description>`, for example `ui/knob-style`, `backend/state-restore` or `dsp/freeze-fade`.
- Open a pull request early, as a draft if it isn't ready yet. Link the issue by writing `Closes #12` in the description.
- A pull request is merged when **CI passes on macOS and Windows**, and the DSP lead has approved it.
- Keep pull requests small. A branch that lives for two days is fine. One that lives for two weeks will be painful to merge.
- Pull `main` into your branch often so conflicts stay small.
- Commit messages: one short line saying what changed, for example "Add input level meters". Add more lines if the reason isn't obvious.

## Who owns what

Changes outside your area are fine, but ask the owner to review them.

| Area | Files | Owner |
|---|---|---|
| DSP engine | `source/VocoderEngine.*`, `tests/` | DSP lead |
| Plugin integration | `source/PluginProcessor.*`, `CMakeLists.txt` | Backend |
| UI | `source/PluginEditor.*`, `source/ui/` | UI pair |
| Build and CI | `CMakePresets.json`, `.github/` | DSP lead |

**UI:** put each component in its own file under `source/ui/` (for example `LookAndFeel.cpp` or `LevelMeter.cpp`), so the two of you aren't both editing `PluginEditor.cpp`. Add new `.cpp` files to `target_sources` in `CMakeLists.txt`.

## Rules that protect users' sessions

- **Parameter IDs are frozen** (the list is in the README). DAW sessions store them. The UI connects to them through `AudioProcessorValueTreeState` attachments, never by copying values around.
- **Nothing that allocates memory, locks or does file I/O runs on the audio thread** (`processBlock` and everything it calls). If you're not sure, ask the DSP lead.
- **Don't change JUCE.** It's pinned to 8.0.14 as a submodule. Upgrading it is a team decision.

## Testing

- CI builds macOS and Windows for every pull request and runs the engine tests. The built VST3 (and the AU on macOS) can be downloaded from the run's **Artifacts** section. That's how Mac users can give a Windows build to a Windows tester, and the other way round.
- Before asking for review on anything that changes sound or parameters, load the plugin in a DAW with a sidechain carrier and check that it still works.
