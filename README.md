# Smart Ducking

Smart Ducking is an OnWave VST3 prototype for voice-aware spatial and spectral ducking in voice-over mixing.

The current `0.2.0` prototype exposes two audio-module classes from one `Smart Ducking.vst3` bundle. `Smart Ducking (Stereo)` has fixed `2 + 6 + 2` buses; `Smart Ducking (Surround)` has fixed `6 + 6 + 6` buses. Both classes use the same processor, DSP, state, and editor sources. Main format follows the selected class, while Voice can be interpreted as Stereo or 5.1 in either class. Stereo sources are decomposed into energy-normalised L/C/R above the Linkwitz–Riley 8th-order crossover; 5.1 sources use direct L/C/R/Ls/Rs components. The Voice components independently control corresponding Variant-A spectral masks, while the adjustable 5×5 matrix calculates spatial wideband gains. LFE is excluded from detection and processing and remains latency-aligned.

The custom editor keeps Main/Voice and wideband/spectral Gain Reduction meters visible while switching between a focused Simple view and Advanced Dynamics, Spectrum, and Routing pages. Meter rows follow the active routing: stereo inputs show L/R, Stereo-class reduction shows its internal L/C/R components, and Surround routing shows L/C/R/Ls/Rs. Voice format is the only format selector. The Surround class additionally exposes forced stereo downmix and optional LFE removal in its header, while VLC Main channel order is available from the Main meter menu; these controls are hidden in the Stereo class. The matrix presents Voice rows and Original columns as shared table headers and dynamically displays only the active 3×3, 3×5, 5×3, or 5×5 set of linear percentage sliders. Lookahead is available in Advanced Dynamics; the current weighting controls remain in Advanced Spectrum until they are replaced by the planned interactive analyser node.

Lookahead is adjustable from 0 to 50 ms with a 10 ms default. Main reserves the full 50 ms while Voice is delayed by `50 ms - Lookahead`, so the reported latency remains `STFT latency + 50 ms` for every Lookahead value. `NORMAL` is the default Processing Mode for new instances. FFT Resolution and Processing Mode changes are queued during playback and applied only through a safe stopped/prepare configuration path instead of resetting DSP state in the audio callback.

## Requirements

- CMake 3.22 or newer
- JUCE 8.0.12 (local checkout, installed CMake package, or automatic FetchContent download)
- Windows: Visual Studio with Desktop development with C++
- macOS: Apple Command Line Tools, CMake, and Ninja

The tracked `CMakePresets.json` contains the common Windows and macOS presets. The ignored `CMakeUserPresets.json` is only for machine-local overrides such as an absolute JUCE path. Without an override, CMake uses `JUCE_PATH`, the workspace-adjacent `../../JUCE-8.0.12`, an installed JUCE package, or downloads the pinned `8.0.12` tag.

## Repository contents

Git tracks the portable `Source`, `Resources`, `Tests`, `docs`, CMake files, and documentation. Generated JUCE code, build trees, IDE state, local presets, plugin bundles, debug symbols, logs, and source archives are ignored. This keeps the same checkout usable on Windows, Apple Silicon, and Intel Mac without committing platform output.

The macOS presets clear `LDFLAGS` for the CMake process so a globally selected `lld` does not replace Apple's linker. `JUCE_PATH` is the only optional environment input and remains the public path to the JUCE checkout, including on Windows. `JUCE_MODULES_DIR` is an internal cache variable created by JUCE after it is loaded; the factory test uses it only to locate JUCE's bundled VST3 SDK. It does not replace or change the meaning of `JUCE_PATH`. `CMAKE_BUILD_TYPE`, `CMAKE_OSX_ARCHITECTURES`, `CMAKE_OSX_DEPLOYMENT_TARGET`, `CMAKE_EXPORT_COMPILE_COMMANDS`, and `SMART_DUCKING_BUILD_TESTS` are CMake cache variables rather than shell environment changes.

## Configure and build

```powershell
cmake --preset local
cmake --build --preset local-release --target SmartDucking_VST3
cmake --build --preset local-release --target SmartDucking_Standalone
cmake --build --preset local-release --target SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset local-release
```

On macOS, build a fast Apple Silicon Debug bundle or a Universal Release bundle:

```sh
cmake --preset macos-debug-arm64
cmake --build --preset macos-debug-arm64 --target SmartDucking_All SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset macos-debug-arm64

cmake --preset macos-release-universal
cmake --build --preset macos-release-universal --target SmartDucking_All SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset macos-release-universal
```

The factory test dynamically loads the finished VST3 and verifies both audio-module classes and their bus layouts. `SmartDucking.Processor` covers the current DSP contract, editor-gated meter capture, and the Simple/Advanced editor with its Dynamics, Spectrum, and Routing pages.

The VST3 bundle is produced under:

```text
build/SmartDucking_artefacts/Release/VST3/Smart Ducking.vst3
```

The Universal macOS VST3 is produced under:

```text
build-macos/release-universal/SmartDucking_artefacts/Release/VST3/Smart Ducking.vst3
```

The build does not copy files into the system VST3 directory.

## Plugin identity

- Company: OnWave
- Website: https://onwavedub.org
- Manufacturer code: `OnWv`
- Stereo component code: `Smdk` (the original component identity is preserved)
- Surround component code: `SmSr`
- Bundle ID: `org.onwavedub.SmartDucking`
- Formats: VST3 and Standalone

## Bus contract

```text
Smart Ducking (Stereo):
Input bus 0: Main Input       Stereo physical bus, required
Input bus 1: Voice Sidechain  5.1 physical bus, required
Output bus 0: Output          Stereo physical bus, required

Smart Ducking (Surround):
Input bus 0: Main Input       5.1 physical bus, required
Input bus 1: Voice Sidechain  5.1 physical bus, required
Output bus 0: Output          5.1 physical bus, required
```

The Voice physical bus is always 5.1. Voice Stereo mode uses its physical L/R and ignores the remaining sidechain pins. Standard 5.1 order is `L R C LFE Ls Rs`; Surround Main can instead use VLC order `L R Ls Rs C LFE`. Surround output is always standard order. Its forced stereo downmix writes L/R and clears C/LFE/Ls/Rs:

```text
Lout = L + 0.707 C + 0.707 Ls + 0.5 LFE
Rout = R + 0.707 C + 0.707 Rs + 0.5 LFE
```

`Drop LFE` removes the final LFE term.

## Implementation status

- [x] Reproducible CMake configuration
- [x] VST3 and Standalone targets
- [x] One VST3 bundle exposing Stereo `2 + 6 + 2` and Surround `6 + 6 + 6` audio-module classes
- [x] Shared processor/DSP/editor code with fixed per-class Main format and selectable Voice format
- [x] Stable parameter IDs and v0.1 defaults
- [x] APVTS state persistence
- [x] Linkwitz–Riley 8th-order crossover with smoothed frequency
- [x] Protected low band and high-band five-channel spatial matrix ducking
- [x] Post-PCE L/C/R level mapping from fixed −60 dBFS to adjustable Voice Full
- [x] Independent Attack/Hold/Release envelopes
- [x] Main/Voice and wideband/spectral GR meters for L/C/R/Ls/Rs
- [x] DSP and state executable tests
- [x] Post-PCE Variant-A spectral suppression on independent Main L/C/R components
- [x] FFT x1/x2/x4 modes and matching host PDC
- [x] Heavy spectral processing sleep on sidechain silence
- [x] Main high-band Phantom Center Extraction sharing the spectral STFT
- [x] Synchronous Voice PCE and post-PCE L/C/R limiting without Voice inverse FFT
- [x] Exact energy-normalised LCR-to-stereo reconstruction
- [x] Stable sidechain silence/one-sided/dual-mono/stereo classifier and extraction fast paths
- [x] Adjustable 5×5 wideband ducking matrix with maximum aggregation
- [x] Standard and VLC Main 5.1 input orders
- [x] Forced stereo downmix with optional Drop LFE
- [x] Adjustable Lookahead with a fixed 50 ms PDC reserve
- [x] Safe stopped/prepare application of FFT Resolution and Processing Mode
- [x] Host Processing Mode names aligned to `QUALITY / NORMAL`
- [x] Obsolete Sidechain Pre-Gain removed with legacy-state migration
- [x] Simple / Advanced interface with focused Dynamics, Spectrum, and Routing pages

Third-party attribution and the Apache-2.0 license for the adapted PCE factor are recorded in `THIRD_PARTY_LICENSES.md`; the build copies this file into the VST3 bundle and next to the Standalone executable.

## Diagnostic logs

Diagnostic builds write one tab-separated snapshot log per plugin instance to `%LOCALAPPDATA%\OnWave\SmartDucking`. A low-priority JUCE thread flushes the file every 1000 ms. The audio callback performs no file I/O or locking; it only updates atomics. Snapshots contain audio-block/sample counters, sidechain-active blocks, FFT mode and PDC, non-finite output count, current sidechain classifier mode, cumulative Voice FFT/PCE counters, and editor paint/timer counters. Sidechain mode values are `0 silence`, `1 left only`, `2 right only`, `3 dual mono`, `4 stereo`, and `5 surround`. This makes it possible to distinguish a stalled REAPER message thread from a stalled audio callback and to verify fast-path selection during profiling.
