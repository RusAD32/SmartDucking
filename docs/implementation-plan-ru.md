# Smart Ducking — сводный план разработки

Статус документа: этапы A–D завершены, план доведён до стабильного релиза `1.0.0`.

Дата актуализации: 13 августа 2026 года.

Прогресс: этап A реализован 10 августа 2026 года, ручной smoke-test сборки в REAPER пройден пользователем 12 августа. Этап B реализован 12 августа и уточнён 13 августа: добавлены независимые stage bypass, взаимно исключающий Monitor Mode, сглаженные paths с постоянной PDC, комплементарные `LOW / HIGH` для Crossover без LFE, фактический индикатор `SLEEP / ACTIVE` и стандартный Global Bypass. LOW/HIGH используют latency-aligned LR8-ветви в QUALITY и реальные комплементарные FFT-веса обработки в NORMAL. Global Bypass сохраняет channel-order routing и Downmix/Drop LFE, а после fade останавливает тяжёлый DSP и прогревает wet path перед возвратом. Этап C реализован 12 августа 2026 года: weighting перенесён на интерактивную ноду, пять конечных matrix controls сглаживаются на частоте STFT, добавлен ручной `L/R LINK`, routing-aware Output meter, сохранение UI-state и короткие переходы Voice/channel-order/Downmix/LFE.

Этап D завершён 12 августа: расширены processor/factory tests, добавлены pluginval/CI и CPU benchmark, вынесены PCE history и frame smoothers, удалён dead UI code, реализованы factory presets и persistent A/B, синхронизирована документация.

Этот документ объединяет решения из первоначальной технической спецификации, результаты уже выполненных этапов, анализ актуального кода и последующие изменения концепции. При расхождении старых разделов `technical-spec-v0.1-ru.md` с этим планом приоритет имеет этот документ. После реализации соответствующего этапа фактическое поведение должно быть перенесено в README, техническую спецификацию и описание архитектуры.

## 1. Цели и ограничения

Основная задача Smart Ducking — объединить в одном оптимизированном плагине обработку оригинальной дорожки и готовой шины закадровых голосов:

1. защита низкочастотной полосы;
2. Phantom Center Extraction для stereo-сигналов;
3. прямое спектральное подавление соответствующих пространственных компонентов;
4. матричный широкополосный дакинг;
5. обратный downmix и восстановление защищённого низа;
6. экономия CPU при отсутствии активного sidechain.

При разработке сохраняются следующие принципы:

- Windows и REAPER остаются основной средой проверки;
- DSP общий для Stereo- и Surround-классов плагина;
- LFE не участвует в детектировании или gain reduction и только выравнивается по задержке;
- спектральная обработка использует вариант A и не следует спектру голоса;
- соседние каналы затрагивает только матрица wideband ducking;
- задержка, заявленная хосту, должна точно соответствовать слышимому тракту;
- SLEEP/ACTIVE, bypass, solo и delta не должны менять PDC;
- аудиопоток не выполняет выделение памяти, файловый ввод-вывод, блокировки или другие непредсказуемые операции;
- оптимизации добавляются после измерений и не должны менять звук незаметно для пользователя.

## 2. Реализованная база

Следующие возможности считаются текущей рабочей базой и не входят в список оставшихся задач:

- один VST3 bundle с классами `Smart Ducking (Stereo)` и `Smart Ducking (Surround)`;
- фиксированные конфигурации шин `2 + 6 + 2` и `6 + 6 + 6`;
- общий DSP, состояние и редактор обоих классов;
- ручной выбор Voice Stereo/Surround;
- ITU/SMPTE, REAPER/VLC, Film и DTS/MPEG-D channel order для Main, Voice Surround и Output без Downmix;
- принудительный stereo downmix и опциональный Drop LFE;
- LR8 crossover и защищённая низкая полоса;
- stereo-to-LCR Phantom Center Extraction для Main и Voice;
- прямое Variant-A spectral reduction в L/C/R/Ls/Rs;
- регулируемая матрица wideband ducking с maximum-агрегацией;
- фиксированная нижняя граница детектора −60 dBFS и регулируемый Voice Full;
- общие Attack/Hold/Release для wideband и spectral stages;
- FFT Resolution ×1/×2/×4;
- два режима обработки с 4× и 2× overlap;
- sidechain-классификатор `silence / left-only / right-only / dual-mono / stereo / surround`;
- быстрые Voice-маршруты без лишнего PCE/FFT;
- сон тяжёлой FFT/PCE/spectral-ветви при тишине sidechain;
- Simple/Advanced интерфейс, интерактивный анализатор, динамические meters и матрица;
- unit-тесты DSP и factory-тест обоих VST3-классов;
- CMake-сборка Debug/Release и основа переносимости на другие ОС.

## 3. Согласованные изменения концепции

### 3.1 Simple и Advanced

Старая идея показывать в Simple только Crossover, Duck Depth, Spectral Amount и FFT Resolution отменена.

Фактическая концепция принимается как основная:

- Simple содержит Ducking Depth, Spectral Depth, Threshold, Attack, Hold и Release;
- Voice Format остаётся глобальным элементом интерфейса;
- Crossover, Spectral Low и Spectral High управляются маркерами анализатора;
- Advanced разделён на Dynamics, Spectrum и Routing;
- FFT Resolution и Processing Mode находятся в Advanced/Spectrum;
- параметры weighting переносятся с отдельных rotary-контролов на анализатор.

### 3.2 Названия Processing Mode

Пользовательский интерфейс остаётся без изменений:

- `QUALITY` — time-domain LR8 и 4× STFT overlap;
- `NORMAL` — complementary FFT-domain crossover и 2× overlap.

Для новых экземпляров default — `NORMAL`. Старые проекты и пресеты сохраняют записанный индекс режима.

В списке параметров DAW значения `Normal / Economy` заменяются на `QUALITY / NORMAL`. ID параметра и индексы вариантов сохраняются, чтобы не менять смысл существующих состояний и автоматизации.

### 3.3 Sidechain Pre-Gain

Sidechain Pre-Gain устарел и должен быть полностью удалён из доступных параметров и DSP. При загрузке старого состояния его legacy-значение безопасно игнорируется или удаляется миграцией состояния.

### 3.4 Lookahead

Существующий параметр Lookahead становится активным. Диапазон остаётся 0–50 ms, default — 10 ms.

Рекомендуемая реализация использует постоянный резерв задержки:

- Main всегда задерживается на 50 ms;
- Sidechain задерживается на `50 ms − Lookahead`;
- относительное опережение sidechain равно выбранному Lookahead;
- итоговая PDC постоянна и равна `STFT latency + 50 ms`;
- изменение Lookahead не требует обновления PDC;
- перемещение sidechain delay tap сглаживается;
- Lookahead размещается в Advanced/Dynamics.

Такая схема намеренно добавляет 50 ms даже при Lookahead 0, но исключает изменение заявленной задержки во время playback и делает автоматизацию безопаснее. Main, low band и LFE должны оставаться точно выровненными.

### 3.5 Weighting EQ как нода анализатора

Weight Frequency, Weight Gain и Weight Q сохраняются как автоматизируемые параметры, но убираются из набора отдельных rotary-контролов.

Они представлены одной нодой параметрического EQ на анализаторе:

- горизонтальное перетаскивание меняет Frequency;
- вертикальное перетаскивание меняет Gain;
- колесо мыши или модификатор при drag меняет Q;
- двойной клик возвращает defaults;
- tooltip показывает Frequency, Gain и Q;
- drag корректно вызывает `beginChangeGesture` и `endChangeGesture` для DAW;
- широкая bell-кривая отображается отдельным цветом или пунктиром;
- интерфейс явно сообщает, что кривая меняет чувствительность детектора, а не слышимый EQ Main или Voice.

Нода не должна конфликтовать с маркерами Crossover, Spectral Low и Spectral High. Полное редактирование weighting доступно в Advanced/Spectrum; в Simple нода может быть скрыта или отображаться без интерактивности.

### 3.6 L/R Symmetry Link

В матрицу добавляется режим `L/R SYMMETRY LINK` для симметричного редактирования.

При изменении `Voice A → Main B` интерфейс устанавливает то же значение в `Voice mirror(A) → Main mirror(B)`, где:

- L ↔ R;
- Ls ↔ Rs;
- C ↔ C.

Примеры:

- C → R связывается с C → L;
- L → C связывается с R → C;
- L → Ls связывается с R → Rs;
- Ls → R связывается с Rs → L.

Это лево-правое пространственное отражение, а не отражение относительно главной диагонали. Главная диагональ ошибочно связала бы C → R с R → C.

Symmetry Link является инструментом редактирования UI, а не частью DSP. Он применяется к ручным изменениям, вводу значения и командам редактирования матрицы, но не должен неожиданно переписывать воспроизводимую DAW-автоматизацию. Состояние кнопки сохраняется как UI-state, а не как автоматизируемый аудиопараметр.

## 4. Безопасная смена конфигурации DSP

Сейчас FFT Resolution и Processing Mode могут сбрасывать STFT/crossover/delay и обновлять latency непосредственно из `processBlock`. Это необходимо устранить до расширения набора режимов.

План:

1. Разделить запрошенную и активную конфигурации DSP.
2. Не выполнять сброс больших DSP-состояний и `setLatencySamples` из аудиопотока.
3. Изменения FFT Resolution применять в безопасной точке, предпочтительно при остановленном transport или при следующем `prepareToPlay`.
4. Если хост меняет FFT Resolution во время playback, сохранить pending-значение и применить его после остановки с явной индикацией ожидания.
5. Для Processing Mode, если latency не меняется, рассмотреть короткий crossfade между старым и новым состояниями. Если двойной engine окажется неоправданно дорогим, применять режим по той же stopped-only схеме.
6. Обновлять PDC на message/configuration thread и уведомлять хост только после готовности новой конфигурации.
7. Сохранять фиксированную Lookahead-часть PDC при любой FFT-конфигурации.
8. Добавить проверки многократного переключения, разных block sizes и остановки/возобновления REAPER.

Voice Format, Main/Voice/Output order, Downmix и Drop LFE не меняют FFT size, но их мгновенное переключение также не должно создавать щелчок. Для них нужен короткий crossfade или применение при остановленном playback, в зависимости от результата прослушивания.

## 5. Сглаживание матрицы

Изменения 25 коэффициентов матрицы сейчас попадают в DSP непосредственно. Требуется сглаживание без 25 независимых тяжёлых smoothers.

Рекомендуемая схема:

1. Рассчитать через актуальную матрицу пять конечных target controls L/C/R/Ls/Rs.
2. Сгладить конечные controls на частоте STFT frames.
3. Использовать сглаженные controls для wideband gain.
4. Не менять direct spectral controls: спектральное подавление по-прежнему не использует матрицу соседства.
5. Подобрать время сглаживания прослушиванием и тестом automation ramps; стартовая область — 20–50 ms.

Проверяются одиночные изменения ячейки, Reset, Diagonal, Symmetry Link, host automation и несколько одновременно активных Voice-каналов.

## 6. Bypass и monitoring основных стадий

Эта возможность реализуется одной связанной итерацией, потому что DSP-маршрутизация и элементы UI должны проектироваться совместно.

Три стадии:

1. Crossover;
2. Wideband;
3. Spectral.

В интерфейсе они располагаются рядом. Первая кнопка каждой стадии — иконка bypass. Crossover получает `LOW / HIGH`, Wideband и Spectral — `S / Δ`.

### 6.1 Bypass

- bypass каждой стадии независим;
- bypass Crossover отключает защиту низа и передаёт full-band сигнал следующим стадиям;
- bypass Wideband отключает матричное широкополосное ослабление;
- bypass Spectral отключает формирование и применение spectral mask;
- тяжёлые вычисления отключённой стадии не выполняются, если они не нужны анализатору или delta monitoring;
- PDC и временное выравнивание не меняются.

### 6.2 Solo и полосы Crossover

- одновременно активен не более чем один Solo;
- Solo оставляет активной выбранную стадию и обходит две остальные;
- Crossover LOW воспроизводит защищённую низкую полосу;
- Crossover HIGH воспроизводит комплементарную обрабатываемую высокую полосу;
- LFE не включается ни в одну Crossover monitor-полосу;
- инфраструктурные PCE, downmix, channel mapping и latency alignment продолжают работать;
- переход Normal ↔ Solo выполняется без щелчка.

### 6.3 Delta

- одновременно активен не более чем один Delta monitor;
- Wideband Delta воспроизводит сигнал, удалённый wideband stage;
- Spectral Delta воспроизводит сигнал, удалённый spectral stage;
- downmix и channel mapping применяются к monitor output предсказуемо;
- включение Delta не меняет PDC.

Для DAW предпочтительна следующая модель параметров:

- три независимых boolean bypass-параметра;
- один choice-параметр Monitor Mode: Normal, Crossover Low, Crossover High, Wideband Solo, Wideband Delta, Spectral Solo, Spectral Delta;
- отдельный boolean Global Bypass, помеченный как стандартный VST3 bypass.

Так host automation не сможет создать неоднозначную комбинацию нескольких Solo/Delta, а UI по-прежнему будет выглядеть как три группы кнопок.

В QUALITY Crossover LOW/HIGH используют delayed LR8 branches. В NORMAL они используют прогретый WOLA path и те же комплементарные FFT-веса, что и реальная обработка; это сохраняет её фазу и реакцию на транзиенты при live switch. Global Bypass плавно обходит processing и monitor paths, сохраняя Main/Output mapping, Downmix и Drop LFE. После fade тяжёлый DSP останавливается; при выходе он прогревается на полную PDC до fade обратно. Параметр сохраняется в project state, но не входит в factory preset и A/B recall.

## 7. SLEEP/ACTIVE и диагностическая индикация

Существующий индикатор наличия sidechain в правом верхнем углу меняет смысл и показывает фактическое состояние тяжёлой ветви:

- `ACTIVE` — FFT/PCE/spectral engine работает, включая hold/release/tail;
- `SLEEP` — тяжёлая ветвь действительно перешла на дешёвый dry path.

Для этого DSP публикует lock-free atomic state. Индикатор не должен переходить в SLEEP сразу после исчезновения сырого sidechain, пока не завершились envelope и overlap-add tail.

Дополнительная Advanced/diagnostic-индикация низкого приоритета:

- активный режим классификатора sidechain;
- PCE L/C/R levels;
- pending FFT/Processing configuration;
- причина, по которой heavy path ещё не уснул.

Логика индикаторов не должна заставлять выполнять FFT только ради GUI. При закрытом редакторе визуализация и дополнительные измерения остаются выключенными.

## 8. Исправления и доработка интерфейса

### 8.1 Output meters

В Surround-плагине при включённом Downmix Output meter должен динамически показывать только L/R. При обычном 5.1 output отображаются L/C/R/Ls/Rs; LFE может оставаться отдельным служебным индикатором только при практической необходимости.

### 8.2 Сохранение UI-state

Между закрытием и повторным открытием редактора сохраняются:

- Simple/Advanced;
- выбранная Advanced page;
- L/R Symmetry Link;
- при необходимости масштаб или другие чисто визуальные настройки анализатора.

Это не должны быть автоматизируемые DSP-параметры.

### 8.3 Presets и A/B

После стабилизации набора параметров добавлены:

- A/B comparison в header: при переключении текущий слот автоматически сохраняется, а второй восстанавливается;
- четыре host factory programs: `Default`, `Gentle Dialogue`, `Voice Focus`, `Fast Announcer`;
- сохранение обоих A/B snapshots, активного слота и factory-program index в project state.

Presets и A/B выполняются после удаления legacy-параметров и фиксации новых IDs Bypass/Monitor Mode.

## 9. Архитектурный рефакторинг

`SpectralSuppressor.h` остаётся orchestration-классом STFT, detector, spectral mask, wideband gain, sleep и визуализации. Под защитой characterization-тестов из него уже вынесены persistent PCE history и переиспользуемые frame/control smoothers. Дальнейшая декомпозиция возможна только малыми шагами без изменения звука.

Предлагаемые компоненты:

- STFT/WOLA engine и geometry;
- Phantom Center Extraction;
- Voice analysis и classifier routing;
- envelope и matrix control;
- spectral prominence/mask;
- crossover-specific processing;
- sleep/wake state;
- spectrum display capture.

Рефакторинг выполняется небольшими шагами с тестом после каждого переноса. Во время него также удаляются:

- неиспользуемые `SpectrumPanel::pushReduction`, `drawHistory` и history buffers, если они не понадобятся новой визуализации;
- неиспользуемые UI components вроде старой centre-кнопки матрицы;
- другие подтверждённые dead paths.

Документация DiagnosticLogger приводится к фактическому периоду записи. Никакая диагностика не пишет файл из audio thread; production build по-прежнему компилируется с отключённой файловой диагностикой.

## 10. Тестирование

### 10.1 Unit и integration tests

Обязательные новые тесты:

- NORMAL/2× overlap для всех FFT Resolution;
- реконструкция импульса и синуса в QUALITY и NORMAL;
- защита низа в обоих Processing Mode;
- wideband и spectral reduction в обоих Processing Mode;
- точная PDC с фиксированным 50 ms Lookahead reserve при 44.1, 48, 88.2 и 96 kHz;
- фактическое опережение 0, 10 и 50 ms;
- автоматизация Lookahead без изменения reported latency;
- отсутствие пропуска начала первой фонемы после SLEEP;
- Bypass/Solo/Delta каждой стадии;
- Crossover LOW/HIGH как комплементарные monitor-полосы без LFE;
- Global Bypass как стандартный host-параметр с постоянной PDC, сохранением routing/downmix и остановкой тяжёлого DSP;
- матричное сглаживание и maximum-агрегация;
- L/R Symmetry mappings для 3×3, 3×5, 5×3 и 5×5;
- сохранение/восстановление новых параметров и UI-state;
- загрузка состояния со старым Sidechain Pre-Gain;
- DAW choice names `QUALITY / NORMAL` при сохранённых индексах;
- динамическое число Output meters при Downmix;
- многократное создание/закрытие editor;
- realtime/offline equivalence;
- случайные и граничные block sizes;
- отсутствие NaN/Inf и allocations в steady-state audio processing.

GUI-тесты должны проверять поведение и доступность controls, а не чрезмерно зависеть от точного числа дочерних Components.

### 10.2 VST3 factory tests

Продолжить проверять оба класса внутри одного bundle:

- корректные имена;
- `2 + 6 + 2` и `6 + 6 + 6`;
- создание processor/editor;
- state round-trip;
- отсутствие конфликта class IDs.

### 10.3 pluginval

pluginval подключается только как dev/CI-инструмент и никогда не входит в VST3, не запускается в REAPER и не создаёт runtime-нагрузку.

Предпочтительная интеграция:

- внешний executable задаётся необязательным `SMART_DUCKING_PLUGINVAL_EXECUTABLE`;
- CMake добавляет отдельный target/test только при наличии этого пути;
- обычные configure/build/test не зависят от pluginval;
- локально и в CI проверяется готовый `Smart Ducking.vst3`;
- оба plugin classes проходят validation;
- strictness 5 используется как обязательный compatibility baseline;
- strictness 10 запускается перед release или по расписанию;
- log сохраняется как CI artifact при ошибке.

Исходники pluginval не линкуются с плагином и не добавляются как обязательный submodule.

### 10.4 REAPER и ручное прослушивание

Минимальная матрица ручной проверки:

- Stereo Main + Stereo Voice;
- Stereo Main + Surround Voice;
- Surround Main + Stereo Voice;
- Surround Main + Surround Voice;
- четыре channel order для Main, Voice Surround и Output без Downmix;
- Downmix с LFE и Drop LFE;
- QUALITY/NORMAL и ×1/×2/×4;
- SLEEP → первая реплика → hold/release → SLEEP;
- playback start/stop, seek, loop, offline render и render stems;
- открытие/закрытие GUI во время playback;
- изменение routing и configuration parameters в допустимых состояниях;
- прослушивание Lookahead, каждого Solo и каждого Delta;
- CPU profiling с закрытым и открытым editor.

Результаты прослушивания фиксируются отдельно от unit-тестов: pluginval и автоматические проверки не оценивают качество PCE, музыкальность spectral reduction или выбранные defaults.

## 11. Документация и согласованность

После каждой функциональной итерации синхронизируются:

- `README.md` — возможности, сборка и краткий status;
- `docs/technical-spec-v0.1-ru.md` — пользовательское и DSP-поведение;
- `docs/ARCHITECTURE.md` — фактический signal flow и realtime-ограничения;
- этот implementation plan — выполненные пункты и изменившиеся решения.

Необходимо отдельно исправить уже известные расхождения:

- описание Simple/Advanced;
- статус weighting controls;
- названия Processing Mode в DAW;
- активный Lookahead и удалённый Sidechain Pre-Gain;
- фактический интервал DiagnosticLogger;
- реализованные Stereo/5.1 возможности, которые старый документ всё ещё относит к future work.

## 12. Рекомендуемая последовательность реализации

### Этап A — безопасность и параметрическая модель — реализовано

1. [x] Добавить тесты NORMAL mode и зафиксировать текущее поведение.
2. [x] Исправить безопасное применение FFT Resolution/Processing Mode.
3. [x] Переименовать DAW choices в `QUALITY / NORMAL`.
4. [x] Удалить Sidechain Pre-Gain и добавить state migration test.
5. [x] Реализовать фиксированный Lookahead reserve и тесты PDC.

### Этап B — управление и мониторинг стадий

1. [x] Ввести параметры Bypass и Monitor Mode.
2. [x] Реализовать DSP Bypass/Solo/Delta для Crossover, Wideband и Spectral.
3. [x] Добавить единую UI-группу с иконкой bypass, `LOW / HIGH` для Crossover и `S / Δ` для Wideband/Spectral.
4. [x] Проверить click-free transitions и неизменную PDC.
5. [x] Перевести верхний индикатор на фактический SLEEP/ACTIVE.
6. [x] Добавить стандартный Global Bypass рядом с SLEEP/ACTIVE и latency-aligned dry path.

### Этап C — анализатор и матрица

1. [x] Перенести Weight Frequency/Gain/Q на интерактивную ноду анализатора.
2. [x] Удалить старые weighting rotary controls.
3. [x] Добавить сглаживание конечных matrix controls.
4. [x] Добавить L/R Symmetry Link.
5. [x] Исправить Output meters при Surround Downmix.
6. [x] Сохранить UI mode/page/link state.
7. [x] Сгладить Voice Format, Main/Voice/Output order, Downmix и Drop LFE.

### Этап D — укрепление проекта — реализовано

1. [x] Расширить unit/factory tests.
2. [x] Подключить внешний pluginval target и CI-проверку.
3. [x] Зафиксировать ручной REAPER smoke-test пользователя и выполнить воспроизводимый CPU profiling.
4. [x] Вынести PCE history и frame/control smoothers из SpectralSuppressor под защитой тестов.
5. [x] Удалить подтверждённый dead UI/DSP code.
6. [x] Синхронизировать всю документацию.
7. [x] Добавить persistent A/B и factory presets после стабилизации parameter schema.

## 13. Критерии готовности ближайшего стабильного релиза

- оба VST3-класса загружаются и стабильно работают в REAPER;
- Debug и Release unit/factory tests проходят;
- pluginval strictness 5 проходит для всего bundle;
- FFT/Processing changes не сбрасывают DSP небезопасно в audio thread;
- PDC равна измеренной задержке во всех FFT modes и sample rates;
- Lookahead действительно создаёт выбранное опережение и не меняет PDC;
- Sidechain Pre-Gain отсутствует в параметрах DAW;
- host показывает `QUALITY / NORMAL`;
- stage/global bypass и monitor modes работают без щелчков и с постоянной задержкой;
- Crossover LOW/HIGH воспроизводят комплементарные полосы без LFE;
- matrix automation не создаёт zipper noise;
- SLEEP/ACTIVE отражает фактическое состояние тяжёлой ветви;
- закрытый GUI не оставляет активной визуализационную FFT-нагрузку;
- realtime и offline render совпадают в пределах численной точности;
- presets старой версии либо мигрируют предсказуемо, либо документированно считаются несовместимыми;
- README, technical spec и architecture соответствуют коду.

## 14. Отложенные возможности

Следующие задачи не входят в ближайший стабильный релиз:

- 7.1 и более широкие layouts;
- AU и AAX;
- дистрибуционные macOS notarization, installer и packaging;
- дополнительный FFT mode 8192 samples при 48 kHz;
- пользовательские параметры Phantom Center Extraction;
- альтернативные кривые между −60 dBFS и Voice Full;
- voice-informed make-space или hybrid spectral ducking — подробный scope,
  safety constraints и исследовательские этапы зафиксированы в
  [`post-1.0-enhancement-plan-ru.md`](post-1.0-enhancement-plan-ru.md);
- отдельная spectral adjacency matrix;
- независимый sleep для каждого пространственного канала без подтверждённой пользы профилированием;
- SIMD-переписывание до появления измеренного bottleneck.

Отдельная spectral matrix не планируется автоматически. Make-space/hybrid
возвращается в работу только после стабильного 1.0 и начинается со скрытого
прототипа без новых host parameters; shipping возможен только при успешном
прослушивании и выполнении критериев post-1.0 плана.
