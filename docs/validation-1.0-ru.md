# Smart Ducking 1.0 — validation record

Дата: 13 августа 2026 года.

Статус: `1.0.0-rc.1`, release candidate; финальный релиз `1.0.0` ещё не зафиксирован.

## Сборка

- версия: `1.0.0`;
- локальная среда: Apple M3 Pro, macOS 26.3.1, 36 GB RAM;
- release artifact: Universal `arm64 + x86_64` VST3;
- подпись: ad-hoc bundle signature, `codesign --verify --deep --strict` проходит;
- установленный bundle: `~/Library/Audio/Plug-Ins/VST3/Smart Ducking.vst3`.

## Автоматические проверки

Debug arm64 и Universal Release собраны полностью. В обеих конфигурациях проходят:

- `SmartDucking.Processor` — DSP, state, editor, routing, monitor modes, Global Bypass, PDC, presets/A-B, extracted smoothers/PCE history, random/boundary block sizes, finite output и realtime/offline-style numerical equivalence;
- `SmartDucking.VST3Factory` — загрузка готового binary, два различных class ID, имена/версия/категория, layouts `2 + 6 + 2` и `6 + 6 + 6`, ровно один параметр с VST3-флагом `kIsBypass`, processing, state, default latency и finite tail 96 000 samples при 48 kHz.

Точная PDC автоматически проверяется при 44.1, 48, 88.2 и 96 kHz. Тест 88.2 kHz добавлен в Stage D. Для NORMAL отдельный transient/multitone regression test проверяет, что `Crossover Low + Crossover High` восстанавливают latency-aligned full-band STFT output: monitor использует ту же FFT-кривую, что и обработка, а не LR8-подстановку. Debug arm64 также проходит с AddressSanitizer и UndefinedBehaviorSanitizer.

## pluginval

Официальный pluginval `1.0.4` запущен для Universal Release bundle с:

```text
--strictness-level 5 --validate-in-process
```

Результат после исправления live LOW/HIGH в NORMAL, расширения channel-order routing и оптимизации Global Bypass: `SUCCESS`, найдены и прошли оба класса. Для Stereo и Surround проверены program list, editor, state, automation, bus negotiation и processing при 44.1/48/96 kHz с block sizes 64–1024. Внутренний optional VST3 SDK validator pluginval не запускался, так как отдельный validator executable не был задан; это не повлияло на успешный итог pluginval.

## CPU benchmark

Метод: Universal Release binary на Apple M3 Pro, 48 kHz, block 512, 128 warm-up blocks и 2000 measured blocks, то есть 21.33 секунды обрабатываемого звука на case. Main получает синус 997 Hz, активный Voice sidechain — синус 283 Hz; в Surround действительно включены шестиканальные Main и Voice. `visualisation-on` включает audio-thread spectrum capture так же, как открытый editor, но не измеряет стоимость GUI paint. Результат `CPU одного ядра` вычисляется как `wall-clock processing time / audio duration × 100`; это не показание CPU meter REAPER.

Каждый диапазон ниже объединяет три FFT Resolution (`x1`, `x2`, `x4`) внутри одного Processing Mode:

| Variant | Mode | Visualisation | Диапазон CPU одного ядра |
|---|---|---|---:|
| Stereo | QUALITY | off | 0.78–0.80% |
| Stereo | QUALITY | on | 0.83–0.87% |
| Stereo | NORMAL | off | 0.45–0.48% |
| Stereo | NORMAL | on | 0.49–0.57% |
| Surround | QUALITY | off | 1.53–1.60% |
| Surround | QUALITY | on | 1.65–1.76% |
| Surround | NORMAL | off | 0.85–0.96% |
| Surround | NORMAL | on | 0.98–1.11% |

Это микробенчмарк конкретной машины без DAW overhead, других плагинов и нескольких экземпляров, а не гарантия CPU на другом hardware. Он служит regression baseline; открытая визуализация даёт измеримую, но небольшую добавку.

Предыдущие диапазоны `0.53–0.81%` не являются сравнимым baseline: helper использовал `setValue()` без host/APVTS notification, поэтому подписи QUALITY/NORMAL/x1/x2/x4 не гарантировали фактическое переключение raw DSP-параметров. Surround-case также оставлял Voice в Stereo и использовал постоянный sidechain вместо тестового сигнала. Текущий benchmark использует `setValueNotifyingHost()`, проверяемые FFT counters и routing, соответствующий подписи case.

Отдельный x2 benchmark подтверждает ветвление monitor path: в QUALITY Crossover Low работает без STFT (`main-fft=0`, `sidechain-fft=0`), а в NORMAL выполняет только main STFT (`main-fft=1064`, `sidechain-fft=0`) и не запускает LR8 или sidechain analysis. Для NORMAL x2 без визуализации steady global bypass измерен в 0.10% одного ядра для Stereo и 0.21% для Surround; FFT counters остаются нулевыми.

## REAPER и listening boundary

Пользователь подтвердил успешный ручной smoke-test в REAPER 12 августа. Это подтверждает фактическую загрузку и работу плагина в целевом хосте. Автоматика дополнительно покрывает оба plugin classes, Stereo/Surround Voice, четыре Main/Voice/Output channel order, Downmix/Drop LFE, QUALITY/NORMAL, live LOW/HIGH switch, global-bypass sleep/warmup, x1/x2/x4, sleep/wake, variable blocks и state. Новая сборка с этими уточнениями ожидает отдельного ручного подтверждения и поэтому публикуется только как release candidate, а не как финальный `1.0.0`.

Полная subjective listening matrix — речь/музыка/эффекты, seek/loop, realtime/offline render, stems и каждый Solo/Delta — остаётся отдельной продуктовой приёмкой. Она не подменяется unit tests и pluginval и в этом record не отмечена как пройденная полностью.

## Realtime-safety audit

В steady-state `processBlock` нет файловых операций, mutex/wait, создания FFT или resize больших контейнеров. Буферы, delay lines, histories и FFT geometry предвыделяются в prepare/configuration path. Обмен с editor и диагностикой ограничен atomics и fixed snapshots; закрытый editor отключает spectrum capture.
