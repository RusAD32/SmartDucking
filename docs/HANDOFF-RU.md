# Smart Ducking — handoff для разработчика или агента

Этот документ предназначен для быстрого погружения нового участника в проект. Он описывает назначение плагина, текущее состояние реализации, ключевые архитектурные решения, карту исходников, сборку, тестирование, известные риски и безопасную точку продолжения работы.

## 1. С чего начать

Рекомендуемый порядок чтения:

1. этот handoff;
2. [`implementation-plan-ru.md`](implementation-plan-ru.md) — согласованный план дальнейшей разработки;
3. [`ARCHITECTURE.md`](ARCHITECTURE.md) — снимок текущего DSP signal flow;
4. [`../README.md`](../README.md) — сборка, identity и краткий статус;
5. [`technical-spec-v0.1-ru.md`](technical-spec-v0.1-ru.md) — первоначальная подробная спецификация.

Важно: старая техническая спецификация частично устарела. Для фактического текущего поведения источником истины является код и тесты. Для будущих изменений приоритет имеет `implementation-plan-ru.md`.

Перед любой работой:

```powershell
git switch main
git status
git pull --ff-only
cmake --list-presets=all
```

Не начинайте изменение кода, пока не понятно, какие локальные или staged-изменения уже принадлежат другому разработчику.

## 2. Назначение продукта

Smart Ducking — VST3 для сведения закадровой озвучки и дубляжа. Он принимает:

- Main — оригинальную дорожку фильма, сериала или другого контента;
- Voice Sidechain — готовый микс голосов озвучки.

Плагин освобождает место для Voice двумя способами:

1. спектрально подавляет выраженные компоненты Main в основном в речевом диапазоне;
2. выполняет пространственно-зависимый широкополосный дакинг Main.

Низкая полоса Main защищена от обработки. Stereo Main и Stereo Voice преобразуются во внутренние L/C/R-компоненты через Phantom Center Extraction. Surround обрабатывается по исходным L/C/R/Ls/Rs. LFE исключён из детектирования и gain reduction.

Главная продуктовая цель — получить более естественный результат, чем у обычного sidechain compressor, при меньшем CPU и более простом routing, чем у цепочки сторонних плагинов.

## 3. Текущий релизный снимок

- Версия CMake-проекта: `0.2.0`.
- Основная ветка: `main`.
- Репозиторий: `RusAD32/SmartDucking`.
- Язык: C++20.
- Framework: JUCE `8.0.12`.
- Основной формат: VST3.
- Дополнительный target: Standalone.
- Основная DAW: REAPER на Windows.
- В одном `Smart Ducking.vst3` зарегистрированы два audio-module class.

Последняя проверенная кодовая база перед созданием документов собиралась в Debug и Release; `SmartDucking.Processor` и `SmartDucking.VST3Factory` проходили в обеих конфигурациях. После получения репозитория всё равно необходимо повторить baseline build и tests на своей машине.

## 4. Plugin classes и шины

### Smart Ducking (Stereo)

```text
Input 0: Main Input       2 channels
Input 1: Voice Sidechain  6 channels
Output : Output           2 channels
```

Физическая схема: `2 + 6 + 2`.

### Smart Ducking (Surround)

```text
Input 0: Main Input       6 channels
Input 1: Voice Sidechain  6 channels
Output : Output           6 channels
```

Физическая схема: `6 + 6 + 6`.

Main format определяется выбранным plugin class и не является параметром. Voice Sidechain физически всегда 5.1, но параметр Voice Format интерпретирует его как:

- Stereo — используются только физические L/R;
- Surround — используются L/R/C/Ls/Rs, LFE игнорируется.

Стандартный порядок 5.1:

```text
L R C LFE Ls Rs
```

VLC Main mode:

```text
L R Ls Rs C LFE
```

Surround output всегда возвращается в стандартном порядке. Forced downmix:

```text
Lout = L + 0.707 C + 0.707 Ls + 0.5 LFE
Rout = R + 0.707 C + 0.707 Rs + 0.5 LFE
```

Drop LFE удаляет последние слагаемые.

## 5. Почему в одном VST3 находятся два плагина

JUCE обычно экспортирует один audio-module class. Проект заменяет только stock VST3 wrapper собственным composite factory из `Source/SmartDuckingVST3Client.cpp`.

Factory:

- сохраняет Stereo component identity `Smdk`;
- добавляет Surround component identity `SmSr`;
- перед созданием processor временно выбирает `PluginVariant::stereo` или `PluginVariant::surround`;
- использует общий `SmartDuckingAudioProcessor`, APVTS, DSP и editor;
- оставляет общий controller class.

Критические файлы identity:

- `CMakeLists.txt`;
- `Source/SmartDuckingVST3Client.cpp`;
- `Resources/moduleinfo.json.in`;
- `Tests/VST3FactoryTests.cpp`.

Не менять manufacturer code, plugin codes, CIDs, bundle ID или порядок factory classes без явного решения о несовместимости старых проектов. После любого изменения wrapper/identity обязательно запускать factory test и пересканировать VST3 в REAPER.

## 6. Текущий signal flow

### 6.1 QUALITY: Stereo Main

```text
Stereo Main
  → fixed 50 ms Main delay
  → LR8 crossover
  ├→ low band → STFT-latency delay ───────────────────────────┐
  └→ high band → shared STFT → Main PCE → L/C/R              │
                                → direct spectral reduction   │
                                → matrix wideband reduction   │
                                → LCR-to-stereo downmix       │
  → сумма обработанного high и защищённого low ←─────────────┘
```

### 6.2 QUALITY: Surround Main

```text
L/C/R/Ls/Rs
  → отдельный LR8 crossover каждого канала
  ├→ delayed protected low
  └→ high → shared STFT
           → direct spectral reduction
           → matrix wideband reduction
  → low + processed high

LFE → только latency alignment
```

Surround Main не проходит PCE.

### 6.3 NORMAL mode

NORMAL использует 2× overlap и complementary FFT-domain crossover weighting. Отдельный time-domain low path для основных каналов не суммируется: low/high protection и gains объединяются в спектральной области. LFE всё равно проходит отдельное выравнивание задержки.

`NORMAL` является default для новых экземпляров. Загрузка состояния не переопределяет сохранённый выбор режима.

В текущем коде внутренний boolean называется `economyMode`, но пользовательское название режима — `NORMAL`. Не распространять старое имя Economy в новый UI или host metadata.

### 6.4 Voice

```text
Voice
  → delay `50 ms − Lookahead`
  → stereo/surround interpretation
  → sidechain classifier
  → минимально необходимое число forward FFT
  → при настоящем stereo: Voice PCE
  → independent detector L/C/R/Ls/Rs
  → level mapping −60 dBFS … Voice Full
  → Attack/Hold/Release envelopes
  ├→ direct controls для spectral reduction
  └→ 5×5 matrix → controls для wideband reduction
```

Voice inverse FFT не нужен, потому что sidechain не слышен.

## 7. DSP-контракты, которые нельзя случайно изменить

### 7.1 Phantom Center Extraction

PCE использует адаптированную идею из `willleskowitz/upmix` под Apache-2.0. Атрибуция находится в `THIRD_PARTY_LICENSES.md`.

Текущая реализация использует:

- time-smoothed complex coherence;
- positive phase agreement;
- inter-channel magnitude balance;
- трёхбинное frequency smoothing;
- energy-normalised centre;
- точную алгебраическую реконструкцию Stereo после LCR downmix.

Основная формула:

```text
common = 0.5 × centreFactor × (L + R)
LcrLeft = L − common
LcrCentre = sqrt(2) × common
LcrRight = R − common

L = LcrLeft + LcrCentre / sqrt(2)
R = LcrRight + LcrCentre / sqrt(2)
```

Stereo Main всегда проходит PCE при активном тяжёлом пути. Для Voice classifier может безопасно выбрать более дешёвый маршрут.

### 7.2 Sidechain classifier

Режимы:

```text
silence, leftOnly, rightOnly, dualMono, stereo, surround
```

- leftOnly/rightOnly используют один forward FFT и прямой L/R detector;
- dualMono использует один forward FFT и направляет результат преимущественно в C;
- stereo использует два FFT и PCE;
- переходы между fast routes защищены hold около 100 ms;
- при сомнении classifier возвращается в безопасный stereo route.

Не добавлять classifier к Stereo Main: там PCE нужен всегда, потому что оригинал почти никогда не бывает чистым mono или hard-panned сигналом на протяжении всей сцены.

### 7.3 Detector и envelope

- уровень ниже −60 dBFS даёт control 0;
- Voice Full, default −30 dBFS, даёт control 1;
- между границами используется линейная интерполяция в dB;
- Attack/Hold/Release общие для wideband и spectral depth;
- дополнительного audible attack внутри spectral mask быть не должно;
- случайное двойное сглаживание уже было признано нежелательным.

### 7.4 Spectral reduction

- только Variant A;
- Voice задаёт активность и величину, но не выбирает частоты;
- подавляемые частоты выбираются по prominence Main;
- weighting bell повышает чувствительность в выбранной области;
- suppression применяется только к соответствующему пространственному каналу;
- матрица соседства на spectral stage не действует;
- default band: 150 Hz–20 kHz;
- default weighting: 3 kHz, +6 dB, Q 0.8;
- `Spectral Amount = 0` должен обходить лишнюю mask-работу.

### 7.5 Wideband matrix

Порядок внутренних spatial channels всегда:

```text
0 L, 1 C, 2 R, 3 Ls, 4 Rs
```

Строки матрицы — Voice detectors, столбцы — Main targets. Для target используется maximum, а не сумма:

```text
target[j] = max_i(detector[i] × matrix[i][j])
```

Default matrix:

| Voice ↓ / Main → | L | C | R | Ls | Rs |
|---|---:|---:|---:|---:|---:|
| L  | 1.00 | 0.25 | 0.00 | 0.35 | 0.00 |
| C  | 0.10 | 1.00 | 0.10 | 0.05 | 0.05 |
| R  | 0.00 | 0.25 | 1.00 | 0.00 | 0.35 |
| Ls | 0.35 | 0.00 | 0.00 | 1.00 | 0.05 |
| Rs | 0.00 | 0.00 | 0.35 | 0.05 | 1.00 |

Не транспонировать матрицу и не суммировать несколько Voice-каналов: это изменит согласованный звук.

### 7.6 Sleep/wake

При тишине sidechain engine продолжает дешёвый dry WOLA path, чтобы PDC не менялась. FFT, PCE, prominence, mask и inverse FFT спят. После исчезновения Voice тяжёлый путь остаётся активен до завершения hold, release и overlap-add tail.

При закрытом editor spectrum visualization выключается. Не выполнять display FFT только ради meter или диагностической анимации.

## 8. Карта исходников

| Файл | Ответственность |
|---|---|
| `Source/PluginProcessor.cpp/.h` | Шины, APVTS, channel mapping, crossover routing, LFE/downmix, PDC, meters, state |
| `Source/SpectralSuppressor.h` | STFT/WOLA, PCE, Voice analysis, spectral mask, wideband gain, sleep и spectrum capture |
| `Source/LinkwitzRiley8Crossover.h` | Сглаживаемый LR8 из четырёх biquad stages на канал |
| `Source/LcrDuckMatrix.h` | 5×5 defaults и maximum aggregation |
| `Source/SidechainClassifier.h` | Silence/one-sided/dual-mono/stereo fast-path selection |
| `Source/SidechainEnvelope.h` | dB mapping и Attack/Hold/Release envelope |
| `Source/PluginEditor.cpp/.h` | OnWave LookAndFeel, Simple/Advanced UI, spectrum, meters, routing matrix |
| `Source/ParameterIds.h` | Стабильные host parameter IDs |
| `Source/SmartDuckingVST3Client.cpp` | Composite VST3 factory с двумя plugin classes |
| `Source/DiagnosticLogger.cpp/.h` | Lock-free counters и отдельный low-priority file writer в diagnostic builds |
| `Tests/ProcessorTests.cpp` | DSP, routing, state, latency, editor и optimization tests |
| `Tests/VST3FactoryTests.cpp` | Динамическая загрузка bundle и проверка VST3 classes/layouts |
| `Resources/moduleinfo.json.in` | Manifest трёх VST3 classes: два components и controller |

`SpectralSuppressor.h` слишком велик и должен быть позднее разделён, но только после добавления characterization tests. Не начинайте знакомство с проектом с широкого механического рефакторинга этого файла.

## 9. Параметры и совместимость состояния

Параметры создаются в `SmartDuckingAudioProcessor::createParameterLayout()`. ID находятся только в `ParameterIds.h` и должны считаться публичным контрактом с DAW.

Правила:

- не переименовывать существующий ID ради косметики;
- display name и choice text можно менять, сохраняя ID и порядок вариантов;
- не менять порядок choice values без migration;
- compatibility test фиксирует отображение старых индексов Processing Mode: `Normal` (0) → `QUALITY`, `Economy` (1) → `NORMAL`;
- новый автоматизируемый control получает отдельный стабильный ID;
- чисто UI-state не следует превращать в host parameter;
- после изменения layout тестировать state round-trip и старый REAPER project/preset.

Lookahead является активным параметром 0–50 ms. Он меняет задержку Voice внутри постоянного 50 ms reserve и поэтому не меняет PDC. `sidechainPreGainDb` удалён из layout; loader удаляет одноимённый legacy child из старого APVTS state.

Текущий `parameterVersion` равен 1.

## 10. Интерфейс

Текущая концепция:

- resizable editor;
- глобальные Main/Voice/Output meters и Wide/Spectral GR;
- Simple с Duck Depth, Spectral Depth, Threshold, Attack, Hold, Release;
- Advanced/Dynamics;
- Advanced/Spectrum;
- Advanced/Routing;
- интерактивные Crossover, Spectral Low и Spectral High markers на spectrum;
- динамическая матрица 3×3, 3×5, 5×3 или 5×5;
- VLC mode доступен из меню Main meter;
- Downmix/Drop LFE видны только в Surround class.

Lookahead уже находится в Advanced/Dynamics. Следующие согласованные изменения описаны в implementation plan:

- weighting EQ как одна draggable parametric node;
- единая группа Bypass/Solo/Delta для Crossover/Wideband/Spectral;
- Crossover Delta слушает защищённый low band, а не математическую разность;
- SLEEP/ACTIVE вместо простого sidechain-present indicator;
- L/R Symmetry Link матрицы;
- Output meter показывает только L/R при forced downmix;
- сохранение Simple/Advanced/page/link UI-state.

## 11. Сборка на новой машине

### 11.1 Требования

- CMake 3.22+;
- JUCE 8.0.12;
- Windows: Visual Studio с workload `Desktop development with C++`;
- macOS: Apple Command Line Tools и Ninja.

Не переносить между машинами старые `build`, `Builds`, `.vs`, `JuceLibraryCode`, `.vst3`, `CMakeCache.txt` или generated project files.

### 11.2 Локальный preset

`CMakeUserPresets.json` игнорируется Git и хранит локальный `JUCE_PATH`. Пример для Windows:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "windows-local",
      "inherits": "windows-vs2026",
      "generator": "Visual Studio 17 2022",
      "binaryDir": "${sourceDir}/build/windows-local",
      "cacheVariables": {
        "JUCE_PATH": "D:/JUCE/JUCE-8.0.12"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "windows-local-debug",
      "configurePreset": "windows-local",
      "configuration": "Debug"
    },
    {
      "name": "windows-local-release",
      "configurePreset": "windows-local",
      "configuration": "Release"
    }
  ],
  "testPresets": [
    {
      "name": "windows-local-debug",
      "configurePreset": "windows-local",
      "configuration": "Debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "windows-local-release",
      "configurePreset": "windows-local",
      "configuration": "Release",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

Строка `generator` нужна при Visual Studio 2022. Для установленной Visual Studio 2026 её можно убрать и использовать generator родительского preset.

Не задавать `JUCE_MODULES_DIR`: это внутренняя переменная JUCE. Публичная настройка проекта — `JUCE_PATH`.

### 11.3 Windows Debug

```powershell
cmake --preset windows-local
cmake --build --preset windows-local-debug --target SmartDucking_All SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset windows-local-debug
```

### 11.4 Windows Release

```powershell
cmake --build --preset windows-local-release --target SmartDucking_All SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset windows-local-release
```

Артефакты:

```text
build/windows-local/SmartDucking_artefacts/Debug/VST3/Smart Ducking.vst3
build/windows-local/SmartDucking_artefacts/Debug/Standalone/Smart Ducking.exe
build/windows-local/SmartDucking_artefacts/Release/VST3/Smart Ducking.vst3
build/windows-local/SmartDucking_artefacts/Release/Standalone/Smart Ducking.exe
```

Сборка намеренно не копирует VST3 в системную папку. Для REAPER укажите build artifact как scan path или скопируйте bundle вручную только после успешных тестов.

### 11.5 macOS

Общие presets уже находятся в `CMakePresets.json`:

```sh
cmake --preset macos-debug-arm64
cmake --build --preset macos-debug-arm64 --target SmartDucking_All SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset macos-debug-arm64

cmake --preset macos-release-universal
cmake --build --preset macos-release-universal --target SmartDucking_All SmartDuckingTests SmartDuckingVST3FactoryTests
ctest --preset macos-release-universal
```

macOS infrastructure существует, но полноценная продуктовая проверка и packaging ещё считаются future work.

## 12. Тестовый цикл после изменений

Минимум для каждого DSP/parameter/editor change:

1. `git diff --check`;
2. Debug build всех targets;
3. Debug `ctest`;
4. Release build всех targets;
5. Release `ctest`;
6. `git status` и просмотр полного diff;
7. для routing, latency, editor или factory changes — ручной smoke test в REAPER.

Перед release дополнительно:

- pluginval strictness 5 для готового bundle;
- strictness 10 как расширенная проверка;
- playback, seek, loop и offline render;
- все четыре комбинации Main/Voice Stereo/Surround;
- standard/VLC order;
- Downmix/Drop LFE;
- QUALITY/NORMAL и ×1/×2/×4;
- GUI open/closed CPU comparison.

pluginval будет подключён как отдельный dev/CI executable. Он не должен входить в VST3, запускаться в REAPER или создавать runtime-нагрузку.

## 13. Realtime safety

В `processBlock` и вызываемом им DSP запрещены:

- heap allocations и resize больших containers;
- mutex, wait и блокирующая синхронизация;
- файловый ввод-вывод;
- создание/уничтожение FFT и других тяжёлых объектов;
- вызовы GUI;
- непредсказуемые системные операции;
- небезопасный reset или изменение PDC во время обработки.

Допустимый обмен с UI:

- atomics;
- заранее выделенные fixed buffers;
- sequence-counter snapshot для spectrum;
- configuration handoff, применяемый вне audio callback.

Все delay lines, FFT buffers, windows и histories выделяются в prepare/configuration stage. SLEEP не освобождает память и не меняет geometry.

## 14. Известные проблемы на момент handoff

Stage A реализован: NORMAL покрыт characterization-тестами, FFT/Processing changes не реконфигурируют DSP в audio callback, host names используют `QUALITY / NORMAL`, Sidechain Pre-Gain удалён с migration test, а Lookahead работает с постоянным 50 ms PDC reserve.

Оставшиеся известные проблемы:

1. Матрица применяется без сглаживания automation changes.
2. Weight Frequency/Gain/Q занимают отдельные controls вместо ноды анализатора.
3. Surround Output meter при forced downmix может показывать лишние каналы.
4. Voice Format, VLC, Downmix и Drop LFE переключаются без transition smoothing.
5. Simple/Advanced page и часть UI-state не сохраняются между editor instances.
6. `SpectralSuppressor.h` объединяет слишком много ответственностей.
7. В editor остаются подтверждённые dead history/components.
8. Bypass/Solo/Delta, SLEEP/ACTIVE и L/R Symmetry Link ещё не реализованы.

Полное решение и порядок находятся в `implementation-plan-ru.md`.

## 15. Рекомендуемая точка продолжения

Stage A завершён. Следующая функциональная итерация — Stage B из implementation plan:

1. ввести три bypass-параметра и взаимоисключающий Monitor Mode;
2. реализовать Bypass/Solo/Delta для Crossover, Wideband и Spectral;
3. Crossover Delta должен воспроизводить защищённый low band без LFE;
4. сохранить PDC во всех monitor modes и добавить click-free transitions;
5. объединить controls трёх стадий в одну UI-группу;
6. перевести верхний индикатор на фактический SLEEP/ACTIVE.

После каждого пункта сохранять маленький проверяемый diff. Не смешивать рефакторинг `SpectralSuppressor.h`, изменение звучания и redesign UI в одном коммите.

## 16. Как диагностировать проблему

### Плагин не появляется в REAPER

1. Проверить, что собран `SmartDucking_VST3`, а не только SharedCode.
2. Проверить путь к целой директории `Smart Ducking.vst3`.
3. Запустить `SmartDuckingVST3FactoryTests`.
4. Проверить `moduleinfo.json` внутри bundle.
5. Выполнить rescan в REAPER.

### Видна только одна версия плагина

Проверить composite factory, Surround CID и generated `moduleinfo.json`. Не заменять `SmartDuckingVST3Client.cpp` stock JUCE client.

### Звук есть до заявленной latency или LFE не выровнен

Проверить одновременно:

- `StereoSpectralLcrProcessor::getLatencySamples()`;
- `lowBandDelay`;
- отдельный LFE delay;
- значение `setLatencySamples()`;
- текущий FFT Resolution и Processing Mode.

К STFT latency всегда добавляется реализованный фиксированный 50 ms Lookahead reserve.

### CPU не падает при тишине sidechain

Проверить:

- classifier действительно видит technical silence;
- envelopes дошли до нуля после hold/release;
- spectral tail завершён;
- editor visualization не заставляет выполнять лишний analysis;
- `Spectral Amount = 0` использует bypass mask path.

### GUI freeze или crash

Сначала определить, продолжает ли audio thread воспроизведение. Проверить editor timer/paint, spectrum snapshot и lifetime Components. Не писать в GUI из audio thread. DiagnosticLogger при включённой diagnostic build пишет файл только с отдельного low-priority thread; production compile definition сейчас отключает диагностику.

## 17. Handoff checklist перед передачей следующему участнику

- рабочее дерево и staged changes описаны;
- текущая ветка и последний commit указаны;
- Debug/Release build status известен;
- failing tests перечислены с полным выводом;
- ручные REAPER-проверки перечислены отдельно от автоматических;
- изменённые parameter IDs или state migrations описаны;
- измеренная PDC записана для затронутых modes/sample rates;
- новые third-party зависимости и лицензии добавлены в attribution;
- implementation plan и фактическая документация обновлены;
- следующий шаг сформулирован как небольшой проверяемый результат.
