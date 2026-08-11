# Smart Ducking 1.0 — validation record

Дата: 12 августа 2026 года.

## Сборка

- версия: `1.0.0`;
- локальная среда: Apple M3 Pro, macOS 26.3.1, 36 GB RAM;
- release artifact: Universal `arm64 + x86_64` VST3;
- подпись: ad-hoc bundle signature, `codesign --verify --deep --strict` проходит;
- установленный bundle: `~/Library/Audio/Plug-Ins/VST3/Smart Ducking.vst3`.

## Автоматические проверки

Debug arm64 и Universal Release собраны полностью. В обеих конфигурациях проходят:

- `SmartDucking.Processor` — DSP, state, editor, routing, monitor modes, PDC, presets/A-B, extracted smoothers/PCE history, random/boundary block sizes, finite output и realtime/offline-style numerical equivalence;
- `SmartDucking.VST3Factory` — загрузка готового binary, два различных class ID, имена/версия/категория, layouts `2 + 6 + 2` и `6 + 6 + 6`, processing, state, default latency и finite tail 96 000 samples при 48 kHz.

Точная PDC автоматически проверяется при 44.1, 48, 88.2 и 96 kHz. Тест 88.2 kHz добавлен в Stage D.

## pluginval

Официальный pluginval `1.0.4` запущен для Universal Release bundle с:

```text
--strictness-level 5 --validate-in-process
```

Результат: `SUCCESS`, найдены и прошли оба класса. Для Stereo и Surround проверены program list, editor, state, automation, bus negotiation и processing при 44.1/48/96 kHz с block sizes 64–1024. Внутренний optional VST3 SDK validator pluginval не запускался, так как отдельный validator executable не был задан; это не повлияло на успешный итог pluginval.

## CPU benchmark

Метод: 48 kHz, block 512, 128 warm-up blocks, 2000 measured blocks, активный Main и sidechain. Каждая комбинация Stereo/Surround × QUALITY/NORMAL × x1/x2/x4 измерена с закрытой и открытой визуализацией.

| Variant | Visualisation | Диапазон CPU одного ядра |
|---|---|---:|
| Stereo | off | 0.53–0.59% |
| Stereo | on | 0.59–0.64% |
| Surround | off | 0.68–0.69% |
| Surround | on | 0.79–0.81% |

Это микробенчмарк конкретной машины, а не гарантия DAW CPU на другом hardware. Он служит regression baseline; открытая визуализация даёт измеримую, но небольшую добавку.

## REAPER и listening boundary

Пользователь подтвердил успешный ручной smoke-test в REAPER 12 августа. Это подтверждает фактическую загрузку и работу плагина в целевом хосте. Автоматика дополнительно покрывает оба plugin classes, Stereo/Surround Voice, standard/VLC mapping, Downmix/Drop LFE, QUALITY/NORMAL, x1/x2/x4, sleep/wake, monitor paths, variable blocks и state.

Полная subjective listening matrix — речь/музыка/эффекты, seek/loop, realtime/offline render, stems и каждый Solo/Delta — остаётся отдельной продуктовой приёмкой. Она не подменяется unit tests и pluginval и в этом record не отмечена как пройденная полностью.

## Realtime-safety audit

В steady-state `processBlock` нет файловых операций, mutex/wait, создания FFT или resize больших контейнеров. Буферы, delay lines, histories и FFT geometry предвыделяются в prepare/configuration path. Обмен с editor и диагностикой ограничен atomics и fixed snapshots; закрытый editor отключает spectrum capture.
