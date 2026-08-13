# Architecture snapshot — v1.0

This file records the implementation boundary of the `1.0.0-rc.2` release candidate. The implementation plan and executable tests define the detailed behavioural contract.

## Spectral/LCR engine

In QUALITY, Main is split with a minimum-phase Linkwitz–Riley 8th-order crossover. Each branch uses four biquads because JUCE's ready-made Linkwitz–Riley processor provides up to a 4th-order response, rather than the required 8th order. The low branch is delayed and recombined without gain reduction. In NORMAL, the split uses complementary weights inside the shared STFT and the external LR8 remains asleep. One synchronised STFT engine analyses Main and Voice on the same windows. Both are decomposed into L/C/R first; Voice L/C/R detectors then control direct L/C/R spectral masks and the maximum-aggregated wideband matrix before Main is reconstructed.

`SpectralSuppressor` remains the orchestration boundary, while persistent PCE state and reusable frame/control smoothing live in `PceHistory.h` and `SpectralFrameSmoothers.h`. The extraction is protected by characterization tests and does not change the signal path.

## Plug-in classes and physical buses

One `Smart Ducking.vst3` bundle registers two audio-module classes. `Smart Ducking (Stereo)` uses fixed `2 + 6 + 2` buses and `Smart Ducking (Surround)` uses fixed `6 + 6 + 6` buses. The VST3 wrapper only selects the processor variant; processor, DSP, parameters, state, and editor implementation are shared. Main format is fixed by the selected class. The common 5.1 Voice sidechain bus is interpreted by the Voice Stereo/Surround selector.

## Main path — QUALITY

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

In NORMAL, low-band protection, high-band processing, LOW/HIGH monitoring, spectrum generation, and mask calculation share the same forward STFT. No duplicate crossover FFT or LR8 monitor substitute is run.

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

During sidechain silence, preallocated per-channel sample rings keep Main history and return the exact STFT-latency-delayed dry signal directly. No window multiply, overlap-add, FFT, prominence analysis, spectral mask, or PCE is performed. After the delay and sidechain histories have drained, a static Surround/NORMAL configuration with closed visualisation advances the same rings in blocks and bypasses per-sample orchestration. Voice activity materialises the retained window, primes overlap-add from the history, and wakes synchronised Main/Voice analysis without changing PDC or losing the first transient. The engine returns to this direct path only after detector hold/release, mask decay, and overlap-add tail complete. `Spectral Amount = 0` bypasses spectral-mask work while retaining LCR matrix ducking.

The fixed Main Lookahead and full-PDC histories share one power-of-two ring with two integer taps. QUALITY low/high alignment uses the same mask-wrapped fixed-delay implementation. The small preallocated memory overhead removes duplicate Main writes, per-sample modulo division, and allocation from steady-state processing; only the continuously variable Voice Lookahead retains an interpolating JUCE delay line.

Weighting-bell values are cached for the active FFT geometry and recalculated only when Weight Frequency/Gain/Q or that geometry changes. Stage bypass and monitor routing skip any LR8, detector, mask, or transform branch that is not needed for audible processing or an explicitly selected monitor.

## Stage routing and monitoring

Three boolean host parameters bypass Crossover, Wideband, and Spectral independently. One choice parameter represents mutually exclusive Crossover Low/High, Wideband Solo/Delta, and Spectral Solo/Delta monitoring. Crossover Low/High use delayed complementary LR8 branches in QUALITY. In NORMAL they run through the already warm WOLA path and apply the exact complementary FFT crossover weights used by NORMAL processing, so monitoring matches its phase and transient response instead of substituting LR8. Both omit LFE and support live switching. Wideband and Spectral Delta subtract the selected wet result from an STFT-aligned dry Main path. Stage and monitor gains use 30 ms ramps, while all paths retain the configured STFT plus Lookahead-reserve latency.

A separate boolean is returned through JUCE's `getBypassParameter()` as the standard VST3 bypass control. It uses a 30 ms crossfade around all ducking stages and monitor modes while preserving the declared PDC. Main input remapping, forced downmix/Drop LFE, and surround output remapping remain active. Once the fade reaches bypass, the crossover, sidechain lookahead/detector, FFT/PCE, matrix, and monitor paths stop advancing; only the fixed latency and requested routing/downmix path runs. Leaving bypass runs that wet path silently for one full reported-latency interval before the 30 ms fade back, avoiding stale DSP output. Global bypass is stored in project state but is excluded from factory-program and A/B snapshot recall.

The spectral engine publishes its real heavy-path state through a lock-free atomic. It remains ACTIVE through detector hold, release, mask decay, and overlap-add tail, and becomes SLEEP only when the direct ring-history path is actually selected. GUI polling does not wake the engine.

Display frames and the analyser use the same fixed logarithmic 20 Hz–20 kHz frequency grid. At sample rates below 40 kHz, display bins above Nyquist remain empty while the visible axis still reaches 20 kHz. Weight Frequency uses the same 20 kHz upper limit, so its interactive curve cannot stop short of the displayed spectrum.

When a host marks processing as non-realtime, spectrum capture and audio-thread meter collection are disabled even if the editor is open. The editor stops dynamic polling and draws one static `RENDERING` overlay until realtime processing resumes.

Voice Stereo/Surround interpretation, four-choice Main/Voice/Output channel order,
forced downmix, and Drop LFE use 30 ms sample ramps. The orders are ITU/SMPTE,
REAPER/VLC, Film, and DTS/MPEG-D. Left-click menus are available on applicable meters,
whose displayed columns follow the physical order. The selected Dynamics/Routing page and matrix-link
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
buffers, histories, delay lines, FFT geometries, and block-path scratch buffers
are prepared before steady-state processing.
