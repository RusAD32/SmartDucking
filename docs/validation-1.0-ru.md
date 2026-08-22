# Smart Ducking 1.0 — validation record

Дата актуализации: 22 августа 2026 года.

Статус: `1.0.0-rc.3`, release candidate; финальный релиз `1.0.0` ещё не зафиксирован.

## Сборка

- версия: `1.0.0`;
- локальная среда: Apple M3 Pro, macOS 26.3.1, 36 GB RAM;
- release artifact: Universal `arm64 + x86_64` VST3;
- подпись: ad-hoc bundle signature, `codesign --verify --deep --strict` проходит;
- установленный bundle: `~/Library/Audio/Plug-Ins/VST3/Smart Ducking.vst3`.

## Автоматические проверки

Debug arm64 и Universal Release собраны полностью. В обеих конфигурациях проходят:

- `SmartDucking.Processor` — DSP, state, editor, routing, monitor modes, Global Bypass, PDC, factory/user presets и A/B, атомарный save/load/overwrite/delete с отбраковкой повреждённых preset-файлов, extracted smoothers/PCE history, random/boundary block sizes, finite output и realtime/offline-style numerical equivalence;
- `SmartDucking.VST3Factory` — загрузка готового binary, два различных class ID, имена/версия/категория, layouts `2 + 6 + 2` и `6 + 6 + 6`, ровно один параметр с VST3-флагом `kIsBypass`, processing, state, default latency и finite tail 96 000 samples при 48 kHz.

RC3 дополнительно покрывает factory/user preset browser, сохранение пользовательских presets, выбранную крутизну Crossover `24/36/48/60/72 dB/oct`, общий комплементарный расчёт NORMAL/UI, LR4/LR6/LR8/LR10/LR12 в QUALITY, tiny smooth-delta macOS trackpad по всей вертикальной drag-зоне маркера и адаптивную геометрию header/двух отдельных карточек второго ряда. Финальная локальная Universal Release сборка прошла `SmartDucking.Processor` и `SmartDucking.VST3Factory` (`2/2`), `codesign --verify --deep --strict`; установленный VST3 побитово совпал со сборочным executable по SHA-256. Пользователь подтвердил итоговую визуальную компоновку перед фиксацией RC3.

Точная PDC автоматически проверяется при 44.1, 48, 88.2 и 96 kHz. Тест 88.2 kHz добавлен в Stage D. Для всех `24/36/48/60/72 dB/oct` отдельный transient/multitone regression test проверяет, что в NORMAL `Crossover Low + Crossover High` восстанавливают latency-aligned full-band STFT output: monitor использует ту же FFT-кривую, что и обработка. Отдельные тесты QUALITY проверяют точку пересечения −6.02 dB, фактическое затухание и плоскую all-pass сумму LR4/LR6/LR8/LR10/LR12. RC2 также сравнивает sample/block sleeping paths, переход `sleep → Voice wake → release → sleep`, переменные размеры блоков и оба integer-delay tap. Debug arm64 проходит с AddressSanitizer и UndefinedBehaviorSanitizer.

После перехода на PFFFT добавлен прямой regression test против `juce::dsp::FFT`: для FFT 512/1024/2048/4096/8192 сравниваются positive-frequency spectra и inverse после изменения всех spectral bins. PFFFT и контрольная JUCE arm64 Release сборки проходят `SmartDucking.Processor`; PFFFT Universal `arm64 + x86_64` bundle и библиотека содержат обе архитектуры и проходят `SmartDucking.Processor`/`SmartDucking.VST3Factory` (`2/2`). Те же два Universal executables отдельно успешно запущены как x86_64 через Rosetta, то есть SSE slice проверен не только линковкой.

## pluginval

Официальный pluginval `1.0.4` запущен для Universal Release bundle с:

```text
--strictness-level 5 --validate-in-process
```

Результат RC2 после оптимизации offline/sleep и фиксированных PDC delay: `SUCCESS`, найдены и прошли оба класса. Для Stereo и Surround проверены program list, editor, state, automation, bus negotiation и processing при 44.1/48/96 kHz с block sizes 64–1024. Внутренний optional VST3 SDK validator pluginval не запускался, так как отдельный validator executable не был задан; это не повлияло на успешный итог pluginval.

## CPU benchmark

Метод: Universal Release binary на Apple M3 Pro, 48 kHz, block 512, 128 warm-up blocks и 2000 measured blocks, то есть 21.33 секунды обрабатываемого звука на case. Main получает синус 997 Hz, активный Voice sidechain — синус 283 Hz; в Surround действительно включены шестиканальные Main и Voice. `visualisation-on` включает audio-thread spectrum capture так же, как открытый editor, но не измеряет стоимость GUI paint. Результат `CPU одного ядра` вычисляется как `wall-clock processing time / audio duration × 100`; это не показание CPU meter REAPER.

Каждый диапазон ниже объединяет три FFT Resolution (`x1`, `x2`, `x4`) внутри одного Processing Mode:

| Variant | Mode | Visualisation | Диапазон CPU одного ядра |
|---|---|---|---:|
| Stereo | QUALITY | off | 0.73–0.80% |
| Stereo | QUALITY | on | 0.75–0.86% |
| Stereo | NORMAL | off | 0.40–0.48% |
| Stereo | NORMAL | on | 0.46–0.57% |
| Surround | QUALITY | off | 1.39–1.74% |
| Surround | QUALITY | on | 1.51–1.72% |
| Surround | NORMAL | off | 0.75–0.86% |
| Surround | NORMAL | on | 0.83–1.07% |

Это микробенчмарк конкретной машины без DAW overhead, других плагинов и нескольких экземпляров, а не гарантия CPU на другом hardware. Он служит regression baseline; открытая визуализация даёт измеримую, но небольшую добавку.

### PFFFT/JUCE A/B

21 августа выполнены три чередующихся arm64 Release-прогона одинакового benchmark binary с compile-time backend `PFFFT` и `JUCE`. Ниже приведены медианы активного x2 path без visualisation; FFT counters и checksum у двух backend совпадают.

| Variant | Mode | JUCE | PFFFT | Изменение throughput |
|---|---|---:|---:|---:|
| Stereo | QUALITY | 131.27x | 153.65x | +17.0% |
| Stereo | NORMAL | 233.08x | 269.97x | +15.8% |
| Surround | QUALITY | 66.28x | 82.29x | +24.2% |
| Surround | NORMAL | 125.23x | 151.07x | +20.6% |

PFFFT является default backend; `SMART_DUCKING_USE_PFFFT=OFF` сохраняет воспроизводимую контрольную JUCE-сборку. Эти цифры ещё не заменяют ручной REAPER render/listening smoke-test нового bundle и Windows/Linux benchmark.

### DSP stage profile после перехода на PFFFT

21 августа активный arm64 Release path дополнительно измерен встроенными high-resolution frame timers и системным sampling profiler. Условия: Apple M3 Pro, 48 kHz, block 512, PFFFT/NEON, FFT Resolution x2, visualisation off, 20 000 measured blocks после warm-up. Доля считается от полного времени `processBlock`, а не только от STFT. Использованная для таблицы первоначальная instrumented build отличалась от обычной Release по throughput не более чем примерно на 2%; x1/x4 дали тот же порядок hotspot'ов с изменением долей в основном на несколько процентных пунктов.

| Операция | Stereo QUALITY | Stereo NORMAL | Surround QUALITY | Surround NORMAL |
|---|---:|---:|---:|---:|
| Frame materialisation + sidechain classification | 5.65% | 5.19% | 2.63% | 2.57% |
| Main forward FFT + Stereo PCE | 7.40% | 6.65% | 5.16% | 5.01% |
| Sidechain FFT + detector/PCE | 11.28% | 10.09% | 8.26% | 7.91% |
| Spectral mask | 24.53% | 14.84% | 20.97% | 13.48% |
| NORMAL frequency-domain crossover/wide gain | 0.17% | 8.76% | 0.02% | 7.61% |
| Inverse FFT + synthesis window | 7.14% | 6.45% | 5.77% | 5.58% |
| Overlap-add/ring advance | 0.76% | 0.90% | 0.67% | 0.85% |
| Frame setup + control smoothing | <0.1% | <0.1% | <0.1% | <0.1% |
| Остальной `processBlock` | 42.93% | 46.97% | 56.43% | 56.90% |

В остаток входят sample-by-sample fixed/lookahead delays, channel-order mapping, output/downmix/bypass mixing и, в QUALITY, внешний time-domain Linkwitz–Riley crossover. Десятисекундные sampling traces обычной Release-сборки уточняют hotspots: чистые PFFFT kernels занимают примерно 11–13% полного DSP; scalar `log10f`/`exp10f` вместе с телом mask/gain loop — примерно 18–26%; вызовы delay line — 3.5–13.4%; явный Surround channel mapping/output routing — ещё 4.6–7.5%. В Stereo QUALITY внешний LR crossover виден отдельно как 9.4%; в Surround значительная часть этого кода инлайнится в `processBlock`.

Audio-thread visualisation capture повышает стоимость активного x2 path примерно на 7–10% в QUALITY и на 16–22% в NORMAL; GUI paint в этих цифрах отсутствует. При silent sidechain NORMAL sleep path стоит лишь около 8–11% активного DSP, steady global bypass — около 17–20%, stage-bypassed path — около 53–59%.

Вывод по пределу оптимизации: замена одной FFT-библиотеки больше не даст крупного выигрыша. При измеренной доле PFFFT kernels даже их гипотетическое ускорение в 2 раза улучшит весь plugin только на 6–7%, а бесплатное FFT дало бы недостижимый потолок около 13–15%. Экспериментальная глобальная `-ffast-math` сборка прошла текущие processor tests и дала медианный throughput +17.3%/+15.0% в Stereo QUALITY/NORMAL и +6.7%/+4.4% в Surround, но компилятор одновременно предупредил о нарушении NaN/Infinity semantics в JUCE. Поэтому production-реализация ниже использует узкий xsimd backend только для подтверждённых mask/gain kernels и сохраняет scalar reference semantics для tail и null-test.

### xsimd vector-math A/B

22 августа реализован переносимый xsimd 14.3.0 backend для трёх тяжёлых участков: power-to-dB magnitude conversion, temporal mask smoothing вместе с `dB -> gain`, а также NORMAL crossover/spectral/wideband gain и optional display-mask conversion. arm64 использует NEON; x86 baseline использует SSE, а AVX2 находится в отдельном translation unit и выбирается только после runtime CPU check. Это не включает глобальный `fast-math` и не меняет Linkwitz–Riley архитектуру QUALITY.

Ниже медианы пяти чередующихся A/B-прогонов на Apple M3 Pro, arm64 Release/PFFFT, 48 kHz, block 512, x2, visualisation off и 20 000 measured blocks. Код, вход, FFT counters и checksum одинаковы; compile-time switch меняет только selected vector-math backend.

| Сценарий | Scalar reference | xsimd/NEON | Throughput | Время CPU на тот же audio |
|---|---:|---:|---:|---:|
| Stereo QUALITY | 213.78x | 229.07x | +7.2% | -6.7% |
| Stereo NORMAL | 354.21x | 377.61x | +6.6% | -6.2% |
| Surround QUALITY | 110.31x | 115.03x | +4.3% | -4.1% |
| Surround NORMAL | 173.21x | 186.60x | +7.7% | -7.2% |

23 августа выполнен дополнительный Windows x64 A/B на Intel Core i7-13700KF. Ниже приведено медианное время обработки блока для scalar reference, baseline xsimd/SSE2 и runtime-dispatched xsimd/AVX2; отрицательная дельта означает снижение CPU time относительно scalar.

| Сценарий | Scalar | xsimd/SSE2 | SSE2 vs scalar | xsimd/AVX2 | AVX2 vs scalar |
|---|---:|---:|---:|---:|---:|
| Stereo QUALITY | 80.50 us | 62.90 us | -21.86% | 59.20 us | -26.46% |
| Stereo NORMAL | 63.00 us | 55.15 us | -12.46% | 50.90 us | -19.21% |
| Surround QUALITY | 141.20 us | 101.90 us | -27.83% | 94.80 us | -32.86% |
| Surround NORMAL | 89.60 us | 76.80 us | -14.29% | 70.00 us | -21.88% |

AVX2 дополнительно снижает медианное время относительно SSE2 на 5.9–8.9%. Проверка готового плагина на той же Windows-системе не выявила функциональных изменений.

Processor tests сравнивают vector kernels со scalar reference на фактических размерах `513/1025/2049/4097` bins. Последний неполный SIMD-пакет обязан совпадать побитово, kernel-ошибка ограничена `1e-5`, а полный scalar/vector spectral processor проверяется в QUALITY/NORMAL и x1/x2/x4 с maximum/relative-RMS границей `2e-5`. Universal `arm64 + x86_64` Release прошёл processor/factory tests (`2/2`), отдельная scalar-control Release — те же `2/2`, новая arm64 Debug сборка прошла AddressSanitizer и UndefinedBehaviorSanitizer. Финальный [GitHub Actions run 32533368960](https://github.com/RusAD32/SmartDucking/actions/runs/32533368960) также полностью зелёный на macOS Universal, Windows x64/MSVC с AVX2 runtime dispatch и Linux x64/GCC (`3/3` jobs, в каждом processor и VST3 factory tests).

### Steady-state block path и Linkwitz–Riley

После профиля реализован отдельный active steady-state path. Он заранее разрешает Main/Voice/Output channel order, обрабатывает два Main latency tap одним блочным проходом, обходит постоянный sidechain delay по каналам, выполняет внешний QUALITY crossover и его alignment delays блоками, затем оставляет минимальный sample loop только для stateful STFT и финального mix. Scratch-память выделяется только в `prepareToPlay()`. При cutoff/slope/routing/lookahead automation, monitor/bypass crossfade, crossover warm-up, visualisation, diagnostics или block size сверх подготовленной ёмкости код автоматически возвращается в прежний generic path.

Linkwitz–Riley не удалён и его Butterworth-squared архитектура не изменена. Для устойчивых коэффициентов добавлены fixed-order kernels по slope и SIMD-пары каналов. Double-precision filter states загружаются в SIMD-регистры один раз на блок, остаются там на всём блоке и записываются обратно перед возвратом; поэтому нет per-sample gather/scatter. Stereo обрабатывается одной парой L/R, Surround — парами L/C и R/Ls плюс scalar Rs. Целевой arm64 benchmark при 48 dB/oct дал `2.140x` ускорение LR (`7.474 -> 3.493 ns/channel-sample`). Для всех пяти вариантов Surround результат составил `1.568x`, `2.037x`, `2.140x`, `2.071x`, `2.131x` соответственно для 24/36/48/60/72 dB/oct. Universal x86_64/SSE slice отдельно запущен через Rosetta: для Surround/48 dB блочное ядро дало `2.616x` (`9.697 -> 3.706 ns/channel-sample`).

Финальный active x2 A/B на Apple M3 Pro, arm64 Release/PFFFT, block 512, visualisation off, по пять запусков по 10 000 measured blocks:

| Сценарий | До block path | После | Throughput | Время CPU на тот же audio |
|---|---:|---:|---:|---:|
| Stereo QUALITY | 167.91x | 209.32x | +24.7% | -19.8% |
| Stereo NORMAL | 305.26x | 354.83x | +16.2% | -14.0% |
| Surround QUALITY | 85.04x | 109.37x | +28.6% | -22.2% |
| Surround NORMAL | 164.26x | 176.42x | +7.4% | -6.9% |

Это прямой compile-time A/B: пять парных запусков для каждого сценария, одинаковые Release/PFFFT flags и вход, единственная разница — `SMART_DUCKING_ENABLE_STEADY_ACTIVE_PATH=OFF/ON`. Каждый финальный optimized run сообщил `steady-blocks=10128`, то есть fast path использовался на всех 128 warm-up и 10 000 measured blocks. Processor tests сравнивают отдельный fast экземпляр с принудительно generic экземпляром на Stereo/Surround, QUALITY/NORMAL, downmix/channel-order routing, variable block sizes, блоке сверх scratch capacity и cutoff/lookahead/routing automation. Максимально допустимая ошибка полного null-test — `1e-6`, relative RMS — `2e-7`. Фактический максимум SIMD LR относительно scalar generic составил `3.73e-8`; two-tap delay обязан совпадать побитово. Существующие impulse latency, все crossover slopes, monitor, bypass, sleep/wake и routing tests остаются включены.

Diagnostics build теперь отдельно накапливает `main_delay_ticks`, `sidechain_delay_ticks`, `input_routing_ticks`, `crossover_ticks` и `output_routing_ticks`. Active block path в этой сборке намеренно отключён, чтобы instrumentation измеряла полный generic fallback; в production эти timers по-прежнему полностью выкомпилированы. Новые узкие per-sample границы увеличивают стоимость самой diagnostics build примерно на 8–12% относительно неинструментированного generic baseline в проверенных Stereo QUALITY и Surround NORMAL случаях. Поэтому новые поля используются для относительной локализации внутри generic path, а итоговый production A/B выше измерен только обычной Release-сборкой.

Отдельный пятисекундный системный sample уже optimized Surround QUALITY run (`300128` steady blocks) подтвердил смену bottleneck: `processBlockTwoTap` оказался ниже 1% полного DSP вместо прежних примерно 8.8% delay calls; SIMD LR48 занимает около 12.5%; spectral mask с `log10f`/`exp10f` и FFT снова являются главными именованными hotspots. То есть заявленный выигрыш получен именно из выбранных delay/routing/LR участков, а не из случайного отключения STFT: FFT counters в A/B совпадают.

Предыдущие диапазоны `0.53–0.81%` не являются сравнимым baseline: helper использовал `setValue()` без host/APVTS notification, поэтому подписи QUALITY/NORMAL/x1/x2/x4 не гарантировали фактическое переключение raw DSP-параметров. Surround-case также оставлял Voice в Stereo и использовал постоянный sidechain вместо тестового сигнала. Текущий benchmark использует `setValueNotifyingHost()`, проверяемые FFT counters и routing, соответствующий подписи case.

Отдельный x2 benchmark подтверждает ветвление monitor path: в QUALITY Crossover Low работает без STFT (`main-fft=0`, `sidechain-fft=0`), а в NORMAL выполняет только main STFT (`main-fft=1064`, `sidechain-fft=0`) и не запускает time-domain crossover или sidechain analysis.

Slope-matrix того же Crossover Low benchmark подтверждает ожидаемую стоимость. В NORMAL все пять значений используют один FFT и остаются в пределах шума измерения: Stereo `0.28–0.29%`, Surround `0.55–0.56%`. В QUALITY стоимость растёт вместе с числом секций: Stereo от `0.27%` на 24 dB/oct до `0.33%` на 72 dB/oct, Surround от `0.60%` до `0.75%`; стандартные 48 dB/oct дали соответственно `0.29%` и `0.65%`. LR6/LR10 выполняют first-order sections сокращённой арифметикой, а не полным biquad path.

RC2 использует отдельную power-of-two ring-buffer для фиксированных integer PDC delay. Буфер хранит Lookahead и полный PDC как два tap одной истории Main, поэтому исключены дублирующие записи и дорогой modulo на каждом сэмпле; объём предвыделенной памяти намеренно округляется вверх. При 48 kHz/x4 это увеличивает потребление памяти примерно на 0.25 MB на экземпляр по сравнению с минимально возможным ring size.

Финальный RC2 x2 benchmark без визуализации:

- Surround NORMAL, активный 5.1 Voice: `0.75%` одного ядра (`132.51x realtime`);
- Surround NORMAL, silent Voice/SLEEP: `0.05%` (`1889.78x`), FFT counters нулевые;
- Surround steady Global Bypass: `0.12%` (`817.07x`), FFT counters нулевые;
- Stereo NORMAL, silent Surround Voice/SLEEP: `0.04%` (`2411.65x`) после добавления стерео block path; RC2 baseline до этой правки был `0.17%` (`597.85x`).

Цифры имеют обычный разброс wall-clock microbenchmark. Реальный REAPER render дополнительно включает media decoder, host routing и остальной проект.

Ручной offline-render benchmark пользователя в REAPER на одном и том же шестиканальном аудио из видео, без Voice sidechain:

- без плагина: `150x realtime`;
- до RC2 sleep/offline-оптимизаций: `86x`;
- после первого offline/sleep fast path: `119x`;
- итоговая RC2 сборка с power-of-two fixed delays и block history path: `140x`.

Это end-to-end измерение вместе с декодированием видео и работой REAPER, поэтому оно не сравнивается напрямую с микробенчмарком выше. Оно подтверждает практический эффект RC2: разрыв относительно render без плагина сократился с 64x до 10x, а скорость с плагином выросла примерно на 63% относительно исходных 86x.

## REAPER и listening boundary

Пользователь подтвердил функциональный и визуальный smoke-test в REAPER, работу LOW/HIGH и итоговый RC2 offline-render result `140x`. Это подтверждает фактическую загрузку и работу актуальной сборки в целевом хосте. Автоматика дополнительно покрывает оба plugin classes, Stereo/Surround Voice, четыре Main/Voice/Output channel order, Downmix/Drop LFE, QUALITY/NORMAL, live LOW/HIGH switch, global-bypass sleep/warmup, x1/x2/x4, sleep/wake, variable blocks и state. Сборка остаётся release candidate, а не финальным `1.0.0`, до завершения реального рабочего использования и полной продуктовой приёмки.

Полная subjective listening matrix — речь/музыка/эффекты, seek/loop, realtime/offline render, stems и каждый Solo/Delta — остаётся отдельной продуктовой приёмкой. Она не подменяется unit tests и pluginval и в этом record не отмечена как пройденная полностью.

## Realtime-safety audit

В steady-state `processBlock` нет файловых операций, mutex/wait, создания FFT или resize больших контейнеров. Буферы, delay lines, histories, FFT geometry и aligned PFFFT work buffers предвыделяются в prepare/configuration path. Фиксированные Main/PDC delay используют power-of-two storage и block copies; интерполируемый Voice Lookahead остаётся JUCE DelayLine. Обмен с editor и диагностикой ограничен atomics и fixed snapshots; закрытый editor и offline render отключают spectrum/meter capture. При offline render открытый editor показывает статический overlay `RENDERING`, который снимается сразу по host/JUCE state без дополнительного audio block. Reopen/pause stale-state устраняется ожиданием свежего block counter и quiescent-сбросом meters/активной лампы.
