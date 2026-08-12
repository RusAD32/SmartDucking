# Architecture snapshot — v1.0

This file records the implementation boundary of the stable 1.0 release. The implementation plan and executable tests define the detailed behavioural contract.

## Spectral/LCR engine

The current increment splits Main with a minimum-phase Linkwitz–Riley 8th-order crossover. Each branch uses four biquads because JUCE's ready-made Linkwitz–Riley processor provides up to a 4th-order response, rather than the required 8th order. The low branch is delayed and recombined without gain reduction. One synchronised STFT engine analyses Main and Voice on the same windows. Both are decomposed into L/C/R first; Voice L/C/R detectors then control direct L/C/R spectral masks and the maximum-aggregated wideband matrix before Main is reconstructed.

`SpectralSuppressor` remains the orchestration boundary, while persistent PCE state and reusable frame/control smoothing live in `PceHistory.h` and `SpectralFrameSmoothers.h`. The extraction is protected by characterization tests and does not change the signal path.

## Plug-in classes and physical buses

One `Smart Ducking.vst3` bundle registers two audio-module classes. `Smart Ducking (Stereo)` uses fixed `2 + 6 + 2` buses and `Smart Ducking (Surround)` uses fixed `6 + 6 + 6` buses. The VST3 wrapper only selects the processor variant; processor, DSP, parameters, state, and editor implementation are shared. Main format is fixed by the selected class. The common 5.1 Voice sidechain bus is interpreted by the Voice Stereo/Surround selector.

## Main path

```text
Stereo-class Main
  → Linkwitz–Riley 8th-order crossover, default 92 Hz
  ├→ delayed stereo low path
  └→ stereo high path
       → always-on Phantom Center Extraction
       → direct L/C/R spectral suppression
       → LCR wideband ducking matrix
       → LCR-to-stereo reconstruction
  → sum with aligned low path
```

The Stereo Main path does not use dynamic mono/stereo classification. Surround Main bypasses PCE and processes its L/C/R/Ls/Rs high-band components directly; LFE is excluded and only latency-aligned.

## Main Phantom Center Extraction

The PCE factor is adapted from `willleskowitz/upmix` under Apache-2.0, but uses temporally smoothed complex coherence instead of the original single-bin normalised cross magnitude. In each processed FFT bin:

```text
common = 0.5 × centreFactor × (L + R)
LcrLeft = L - common
LcrCentre = sqrt(2) × common
LcrRight = R - common
```

The matching downmix is `L = LcrLeft + C/sqrt(2)` and `R = LcrRight + C/sqrt(2)`. It is an algebraic identity for every value of `centreFactor`, so switching between the dry WOLA path and active extraction cannot colour the signal before LCR gains are applied. `centreFactor` combines true time-smoothed coherence, positive phase agreement, and inter-channel magnitude balance, followed by three-bin frequency smoothing.

## Voice sidechain path

```text
Stereo Voice Sidechain
  → synchronised forward FFT
  → Phantom Center Extraction
  → independent L/C/R peak estimate
  → 0…100% mapping from fixed −60 dBFS to adjustable Voice Full
  → L/C/R Attack/Hold/Release envelopes
  ├→ direct L/C/R spectral control
  └→ maximum-aggregated wideband matrix control
```

The Voice spectrum is used only for PCE and broadband detector magnitude, never for voice-matched frequency selection. No Voice inverse FFT is required. A stable classifier selects silence, left-only, right-only, dual-mono, or stereo routing. One-sided and dual-mono fast paths use one Voice forward FFT, bypass the second FFT and Voice PCE, and preserve the existing detector scale; stereo uses two forward FFTs and PCE. Reported latency never changes.

## Lookahead and reported latency

Main is delayed by a fixed 50 ms reserve before crossover/STFT processing. Voice is delayed by `50 ms - Lookahead`, where Lookahead is adjustable from 0 to 50 ms and defaults to 10 ms. This creates the requested relative detector advance while keeping reported latency fixed at:

```text
reported latency = active STFT size + ceil(0.050 × sample rate)
```

The sidechain delay tap moves over a short smoothing ramp. Main, protected low, processed high, downmix, and LFE remain aligned for every Lookahead value.

The processor reports a finite two-second tail. This upper bound covers maximum Hold/Release configuration plus delayed STFT, crossover ringing, and overlap-add drain, preventing hosts from truncating buffered output after input silence.

FFT Resolution and Processing Mode parameters have requested and active states. During playback the requested state remains pending. Geometry reset and host PDC notification occur only through a stopped/message-thread configuration path or the next `prepareToPlay`, never directly from `processBlock`. If a DAW stops audio callbacks before reporting the stopped state, four unchanged 20 Hz timer observations (about 200 ms of callback quiescence) provide the stopped fallback.

## LCR wideband matrix

| Voice ↓ / Main → | L | C | R |
|---|---:|---:|---:|
| L | 1.00 | 0.25 | 0.00 |
| C | 0.10 | 1.00 | 0.10 |
| R | 0.00 | 0.25 | 1.00 |

Multiple detectors are combined with `maximum`, never by summation.

The matrix first produces five final Main target controls. Those five values,
rather than all 25 coefficients, are smoothed once per STFT frame with a 35 ms
one-pole transition before wideband gain calculation. Direct spectral controls
remain independent of the adjacency matrix. `L/R LINK` is an editor-only spatial
reflection tool for manual changes; it is not a host parameter and never mirrors
DAW automation.

## Spectral contract

- Variant A: the sidechain controls activity and amount only.
- Frequencies to suppress are selected by prominence analysis of the Main signal.
- Voice frequency content does not select the frequencies to suppress.
- Spectral suppression affects only the corresponding L/C/R component after Main PCE.
- One weighting bell, default 3 kHz, +6 dB, Q 0.8.
- FFT Resolution: x1 = 1024, x2 = 2048 (default), x4 = 4096 at 44.1/48 kHz.
- Processing Mode: `NORMAL` is the default for new instances; stored states retain their saved `QUALITY`/`NORMAL` choice.
- Temporal smoothing stabilises only the normalised Main-derived mask shape. The shared L/C/R Attack/Hold/Release envelope is applied afterwards and is the sole smoothing of audible spectral-duck depth. A waking engine initialises the mask shape immediately.

## Silence optimization

Every STFT frame advances the same WOLA state and therefore keeps PDC and audible latency fixed. During sidechain silence, the combined engine advances cheap dry WOLA frames, so FFT, prominence analysis, spectral mask generation, PCE, and inverse FFT sleep. Voice activity wakes synchronised Main and Voice analysis. After the shared L/C/R envelopes fall below their sleep thresholds, the engine returns to the dry path. `Spectral Amount = 0` bypasses spectral-mask work while retaining LCR matrix ducking.

## Stage routing and monitoring

Three boolean host parameters bypass Crossover, Wideband, and Spectral independently. One choice parameter represents mutually exclusive Crossover Low/High, Wideband Solo/Delta, and Spectral Solo/Delta monitoring. Crossover Low/High use the delayed complementary LR8 branches in QUALITY and complementary FFT-domain curves in NORMAL; both omit LFE. Wideband and Spectral Delta subtract the selected wet result from an STFT-aligned dry Main path. Stage and monitor gains use 30 ms ramps, while all paths retain the configured STFT plus Lookahead-reserve latency.

A separate boolean is returned through JUCE's `getBypassParameter()` as the standard VST3 bypass control. It uses a 30 ms crossfade to the raw Main input delayed by the fixed Lookahead reserve and current STFT latency. This path intentionally bypasses VLC remapping, downmix, Drop LFE, all processing stages, and monitor modes while preserving the declared PDC. The wet engine continues advancing so bypass can be left without stale state. Global bypass is stored in project state but is excluded from factory-program and A/B snapshot recall.

The spectral engine publishes its real heavy-path state through a lock-free atomic. It remains ACTIVE through detector hold, release, mask decay, and overlap-add tail, and becomes SLEEP only when the existing dry WOLA fast path is actually selected. GUI polling does not wake the engine.

Display frames and the analyser use the same fixed logarithmic 20 Hz–20 kHz frequency grid. At sample rates below 40 kHz, display bins above Nyquist remain empty while the visible axis still reaches 20 kHz. Weight Frequency uses the same 20 kHz upper limit, so its interactive curve cannot stop short of the displayed spectrum.

Voice Stereo/Surround interpretation, standard/VLC Main order, forced downmix,
and Drop LFE use 30 ms sample ramps. The selected Dynamics/Routing page and matrix-link
choice are stored as non-automatable APVTS root properties. Legacy Simple/Advanced
state migrates to Dynamics, except the old Routing page which remains Routing. The Output meter
exposes five spatial channels for normal Surround output and only L/R while
forced downmix is selected.

## Programs and A/B state

The VST3 factory program list exposes four complete starting points: `Default`,
`Gentle Dialogue`, `Voice Focus`, and `Fast Announcer`. Selecting a program first
restores all parameter defaults and then applies the preset overrides, so no
values leak from the previously selected program.

The editor header has two comparison slots. Switching slots captures the current
normalised value of every processing/routing parameter and restores the other snapshot. Both
snapshots, the active slot, the current factory-program index, and the ordinary
APVTS/UI state are serialized in the project state. Snapshot storage is allocated
outside `processBlock`; global bypass remains independent of comparison recall.

## Validation and realtime boundary

`SmartDucking.Processor` exercises the DSP/state/editor contract, including
random and boundary block sizes and realtime/offline-style numerical
equivalence. `SmartDucking.VST3Factory` loads the produced binary and verifies
both component classes, layouts, metadata, state, processing, and default PDC.
The optional `SmartDuckingPluginval` target validates the finished bundle with
an external pluginval executable; pluginval is never linked into or invoked by
the plug-in. `SmartDuckingBenchmark` measures the full Stereo/Surround,
QUALITY/NORMAL, x1/x2/x4, visualisation-off/on CPU matrix.

The audio callback performs no filesystem access, locking, container resizing,
FFT construction, or GUI calls. Parameters are read through atomics; all large
buffers, histories, delay lines, and FFT geometries are prepared before steady-
state processing.
