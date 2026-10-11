# Smart Ducking

[Русский](README_RU.md) · English

> **AI disclosure:** AI tools were used during the development of Smart Ducking.

Smart Ducking by RusAD makes room for dialogue in music, ambience, and effects. Place it on the background track and feed the voice into its sidechain: the background becomes quieter while someone speaks and returns smoothly between phrases.

## Main features

- **Wideband and spectral ducking:** reduce the background level and selectively soften frequencies that compete with the voice.
- **Stereo and 5.1 surround:** one VST3 bundle provides Smart Ducking (Stereo) and Smart Ducking (Surround), with independent control over how voice channels affect the background.
- **Protected low frequencies:** a crossover keeps the low band out of ducking; LFE stays outside detection and processing.
- **Easy starting points:** four factory presets, saved user presets, and A/B comparison.
- **Visual feedback:** input/output meters, gain-reduction meters, and a spectrum display help you judge the result.

## Download and install

Download the archive for your system from [Releases](https://github.com/RusAD32/SmartDucking/releases/latest): Windows x64, Linux x64, or macOS Universal (Apple Silicon and Intel, macOS 11 or later).

Extract the archive and copy the entire **Smart Ducking.vst3** bundle into your VST3 folder:

| System | VST3 folder |
| --- | --- |
| Windows | `C:\Program Files\Common Files\VST3` |
| macOS | `~/Library/Audio/Plug-Ins/VST3` |
| Linux | `~/.vst3` |

Restart your DAW or rescan its plugins. You need a VST3 host with external sidechain routing; surround use also requires multichannel support. The plugin has been manually checked in REAPER; routing controls vary between hosts.

## Quick start

1. Insert **Smart Ducking (Stereo)** on your stereo music/background track, or **Smart Ducking (Surround)** on a 5.1 track.
2. Send the dialogue track to the plugin’s **Voice Sidechain**, keeping the background on **Main Input**. The sidechain bus has six physical channels even in the Stereo plugin: for stereo voice, send it to sidechain L/R and select **Voice: Stereo**. In REAPER, these are normally track channels 3/4 for the Stereo plugin; check the plugin pin connector.
3. Choose **Gentle Dialogue**, **Voice Focus**, or **Fast Announcer** as a starting point and play a passage with speech.
4. Adjust **THRESHOLD** so speech activates ducking, then set **DUCKING DEPTH** and **SPECTRAL DEPTH** by ear. Use **ATTACK**, **HOLD**, and **RELEASE** to make the transitions feel natural.
5. Compare with bypass or **A / B**. If nothing happens, check that the Voice meters receive the dialogue signal.

On **ROUTING**, the matrix sets how each voice channel reduces each background channel. For 5.1, match the Main, Voice, and Output channel orders to your host. The Surround plugin can also downmix its output to stereo.

**NORMAL** is the default processing mode; **QUALITY** offers more accurate transient response at higher CPU cost. Stop playback before changing Processing Mode or FFT Resolution. Lookahead helps catch the start of words; the plugin reports latency to the host for compensation.

## Help and technical details

Report problems through [GitHub Issues](https://github.com/RusAD32/SmartDucking/issues), including your OS, DAW, plugin version, and routing setup.

Build instructions and implementation details are in the [developer guide](docs/developer-guide.md). Third-party notices are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
