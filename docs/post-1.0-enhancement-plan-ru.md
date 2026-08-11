# Smart Ducking — post-1.0 enhancement plan

Статус документа: согласованный подробный план исследований и улучшений после
стабильного релиза `1.0`.

Дата фиксации: 11 августа 2026 года.

Этот документ не изменяет scope ближайшего стабильного релиза. До завершения
Stage B–D из [`implementation-plan-ru.md`](implementation-plan-ru.md) приоритет
имеет текущий Variant A и существующие DSP-контракты. Работы из этого документа
начинаются только от измеренного и прослушанного baseline `1.0`.

## 1. Зачем нужен этот документ

Во время обсуждения post-1.0 направления исходная формулировка «максимально
подавить оригинальный голос» была уточнена. Подавление оригинального диалога —
это требуемый результат обработки, но не конечная продуктовая цель.

Конечная цель Smart Ducking:

> освободить в оригинальной фонограмме место для готового голоса перевода,
> обеспечить его слышимость и разборчивость и при этом максимально сохранить
> музыку, ambience, Foley, эффекты и общую атмосферу сцены.

Из этого следует другой порядок приоритетов:

1. разборчивость и устойчивое положение Voice в итоговом миксе;
2. минимальное повреждение фонового содержимого Main;
3. снижение заметности оригинальной речи настолько, насколько это помогает
   первым двум пунктам;
4. полное удаление оригинального диалога не является самостоятельной целью.

Оценивать эффект необходимо в первую очередь на итоговой сумме:

```text
Final Mix = Processed Main + audible Translation Voice
```

Solo-сигнал `Processed Main` остаётся полезным диагностическим режимом, но его
самостоятельная естественность не является главным критерием качества, если
финальная сумма звучит убедительно.

## 2. Зафиксированные продуктовые предположения

### 2.1 Ответственность звукорежиссёра

Звукорежиссёр отвечает за:

- панорамирование перевода близко к положению оригинального актёра;
- допустимую ручную корректировку панорамы с учётом слышимого результата
  Smart Ducking;
- громкость, EQ, компрессию и иной творческий processing переводного голоса;
- воспроизведение заметной пространственной обработки оригинала, если для
  сцены нужен room/reverb/delay или художественный эффект вроде «голоса бога»;
- итоговый баланс `Processed Main + Voice`.

Совпадение панорамы не обязано быть математически точным. Небольшая ошибка
допускается и покрывается существующим L/C/R-разложением, распределением
энергии Voice между компонентами и регулируемой wideband matrix.

### 2.2 Routing-контракт Voice

Для алгоритма make-space Voice должен представлять тот голос, который будет
слышен в финальной сумме, с практически теми же:

- панорамой;
- уровнем;
- основным EQ;
- динамической обработкой, существенно меняющей спектральный баланс.

Предпочтительный DAW routing — post-fader/post-pan и, если возможно,
post-processing send. Если рабочий процесс использует другой tap point, разница
должна компенсироваться явной калибровкой или документированным параметром.
Иначе невозможно надёжно оценить требуемый Voice-to-Main spectral margin.

### 2.3 Что намеренно остаётся вне scope

Post-1.0 план не включает:

- нейросетевое stem separation/unmixing;
- полное восстановление M&E, скрытого под оригинальной речью;
- автоматическое исправление заметно неправильной панорамы перевода;
- отдельный spatial tracker оригинального актёра;
- попытку определить точные частоты оригинального актёра по спектру Voice;
- обязательное подавление остатка оригинальной реплики до и после перевода;
- автоматическое копирование reverb/delay оригинального актёра;
- ambience refill как обязательную стадию;
- отдельную spectral adjacency matrix без подтверждённой потребности;
- изменение текущих VST3 identity, bus contract или LFE contract.

Различная длительность оригинальной и переводной реплик, а также небольшие
следы работы Attack/Hold/Release считаются допустимыми. Их улучшение возможно
позднее как nice-to-have, но не должно задерживать основной make-space этап.

## 3. Текущий baseline 1.0, который нельзя потерять

На момент создания плана текущий spectral stage является Variant A:

- Voice задаёт пространственную активность и глубину;
- частоты выбираются по prominence Main;
- одна log-frequency weighting bell повышает чувствительность вокруг заданной
  области;
- spectral reduction применяется напрямую к соответствующему
  L/C/R/Ls/Rs-компоненту и не использует adjacency matrix;
- общая Attack/Hold/Release envelope задаёт слышимую глубину wideband и
  spectral processing;
- отдельное сглаживание spectral shape не должно создавать второй audible
  attack/release;
- защищённый low band, LFE, WOLA, sleep/wake и PDC сохраняют текущие контракты.

Текущую форму можно кратко представить так:

```text
rawVariantA(f, t) = clamp(
    (MainProminence(f, t) + WeightBell(f) - 4 dB) * 1.5,
    0,
    SpectralAmount)

appliedVariantA(f, t) = smoothShape(rawVariantA) * VoiceEnvelope(t)
```

Текущие значения shape smoothing:

- усреднение трёх соседних FFT bins;
- shape attack около 20 ms;
- shape release около 100 ms;
- общий Voice Attack/Hold/Release применяется после shape smoothing.

Default weighting `3 kHz / +6 dB / Q 0.8` — широкая статическая
speech-oriented чувствительность. При этих настройках половина максимального
weighting drive сохраняется ориентировочно между 1.8 и 5 kHz. Поэтому weighting
можно считать сильно упрощённым статическим аналогом make-space mask, но он:

- не следует конкретной реплике;
- не сравнивает уровни Voice и Main;
- использует один bell;
- не знает, в каких perceptual bands Voice реально требует headroom.

Baseline должен оставаться доступным для bitwise/characterization comparison и
безопасного fallback, пока новый режим не пройдёт прослушивание.

## 4. Главное архитектурное решение post-1.0

Следует разделять два разных назначения спектральной обработки.

### 4.1 Translation Make-Space — основной контур

Этот контур отвечает на вопрос:

> в каких perceptual bands и пространственных компонентах текущему Voice нужно
> дополнительное место относительно Main?

Спектр Voice здесь не считается спектром оригинального актёра. Различия языка,
диктора, F0, формант и артикуляции не являются ошибкой алгоритма: make-space
обслуживает именно слышимый переводной голос.

### 4.2 Original Bleed Reduction — дополнительный контур

Текущий Main-prominence Variant A сохраняется как средство уменьшить заметность
оригинальной речи и выраженных компонентов Main во время перевода. Он не должен
быть главным источником spectral damage и может работать с меньшей относительной
глубиной после появления make-space.

### 4.3 Hybrid

Начальный кандидат объединения — максимум в dB, а не сумма:

```text
FinalSpectralReduction(f, t, channel) = max(
    MakeSpaceReduction(f, t, channel),
    OriginalBleedReduction(f, t, channel))
```

Maximum не позволяет двум контурам случайно удвоить глубину. Weighted blend и
другие варианты допустимы только как экспериментальные альтернативы и должны
сравниваться с `max` на одинаковом master depth.

Wideband matrix остаётся отдельной стадией и по-прежнему является fallback для
общего spatial headroom. После успешного make-space прототипа следует проверить,
можно ли уменьшить типичную wideband depth без потери разборчивости Voice.

## 5. Предлагаемый signal flow

```text
Voice L/C/R/Ls/Rs spectra
  → perceptual-band energy
  → Voice band presence
  → smooth spectral envelope
  → spatially direct make-space controls
                                  ┐
Main L/C/R/Ls/Rs spectra           │
  → perceptual-band energy         │
  → prominence / transient data    │
  → Voice-to-Main margin estimate ─┤
                                  ▼
                         Make-Space Reduction

Main prominence → existing Variant A → Original Bleed Reduction

max(Make-Space, Original Bleed)
  → spectral safety constraints
  → existing real non-negative FFT gains
  → existing wideband matrix gain
  → WOLA reconstruction / protected low / aligned LFE
```

Новый анализ Voice должен использовать уже выполняемые forward FFT. Voice
inverse FFT по-прежнему не нужен.

## 6. Perceptual make-space mask

### 6.1 Почему нельзя копировать Voice FFT bin-to-bin

Точная маска отдельных гармоник может создавать:

- движущиеся узкие notches вслед за F0;
- эффект «говорящего EQ» на Main;
- musical noise, warbling или phasiness;
- повреждение устойчивых музыкальных partials;
- temporal smearing транзиентов;
- нестабильность стереообраза при разных масках компонентов.

Текущее усреднение трёх bins недостаточно для точной Voice-маски. При 48 kHz:

- FFT 2048: три bins занимают примерно 70 Hz;
- FFT 4096: три bins занимают примерно 35 Hz.

Для сравнения, perceptual ERB около 3 kHz имеет ширину порядка нескольких сотен
герц. Временное сглаживание снижает дрожание, но не превращает harmonic comb в
естественную широкую make-space область.

### 6.2 Рекомендуемое представление

Начальная архитектура должна использовать 24–32 ERB/Bark-полосы в активном
Spectral Low–High диапазоне. Точное количество не фиксируется до benchmark.

Для каждого spatial component:

1. получить power spectrum Voice;
2. накопить power в perceptual bands через заранее рассчитанные веса;
3. перевести power в dB;
4. вычислить band presence, чтобы полоса без Voice не требовала подавления;
5. сгладить spectral envelope между соседними bands;
6. сгладить shape во времени;
7. интерполировать итоговый gain обратно к FFT bins.

Альтернативный кандидат — cepstral smoothing log-spectrum Voice. Он может быть
добавлен в offline prototype для сравнения, но ERB/Bark принимается как первый
вариант из-за ясной perceptual семантики и удобства ограничения ширины.

### 6.3 Начальная формула

Для spatial component `c` и perceptual band `b`:

```text
voicePresence[c, b] = mapVoiceBandLevel(VoiceDb[c, b])

neededDb[c, b] = clamp(
    MainDb[c, b] - VoiceDb[c, b] + TargetMarginDb[b],
    0,
    MaximumSpectralDepthDb)

makeSpaceTarget[c, b] =
    voicePresence[c, b] * neededDb[c, b]
```

`voicePresence` обязателен. Без него тихий или пустой Voice band ошибочно
потребовал бы максимальное подавление Main из-за большого `Main - Voice`.

Формула является отправной точкой, а не финальным DSP-контрактом. Нужно
сравнить:

- linear и soft-knee band presence;
- flat и speech-importance Target Margin;
- absolute и Voice-envelope-normalised band levels;
- peak, RMS и percentile Main level estimates;
- `max` и confidence-weighted hybrid с Variant A.

### 6.4 Spatial contract

Make-space следует пространственным компонентам Voice так же, как текущий
direct spectral stage. Отдельный поиск панорамы оригинального актёра не нужен.

- Stereo Voice проходит существующий classifier/PCE и даёт L/C/R;
- Surround Voice использует прямые L/C/R/Ls/Rs;
- suppression направляется в соответствующий Main component;
- небольшая ошибка панорамы покрывается распределением Voice между соседними
  компонентами и wideband matrix;
- LFE не анализируется и не подавляется;
- отдельная spectral adjacency matrix не добавляется без результатов
  прослушивания, показывающих реальную необходимость.

## 7. Psychoacoustic и DSP safety constraints

### 7.1 Frequency smoothing

Стартовые ограничения прототипа:

- минимальная эффективная ширина attenuation region: 0.5–1 ERB;
- плавная интерполяция между perceptual bands;
- ограничение максимальной разницы gain соседних bands;
- ограничение максимального spectral slope в dB/octave;
- отсутствие одиночных глубоких FFT-bin notches;
- опциональная median/cepstral стабилизация только после A/B comparison.

Численные limits фиксируются после набора listening examples, а не выбираются
как публичные параметры заранее.

### 7.2 Temporal smoothing без двойного audible envelope

Нужно сохранить текущий принцип:

- shape smoothing стабилизирует только частотную форму;
- общий Voice Attack/Hold/Release определяет слышимую глубину;
- второй полный gain envelope внутри make-space не добавляется;
- при wake shape инициализируется текущим корректным значением, а не старым
  спектром предыдущей реплики;
- release shape может быть длиннее attack shape, но не должен создавать
  дополнительный слышимый хвост поверх общего release.

Начальные 20 ms shape attack и 100 ms shape release используются как baseline,
но должны быть перепроверены для движущейся Voice envelope.

### 7.3 Gain constraints

Обязательны:

- общий master cap через существующий Spectral Amount;
- soft knee около начала attenuation;
- finite-value guards;
- минимальный допустимый spectral gain;
- ограничение frame-to-frame gain delta;
- отсутствие gain выше unity;
- phase-preserving real non-negative multiplication FFT bins;
- одинаковая WOLA geometry и PDC при включении/выключении режима.

### 7.4 Transient protection

Первый make-space prototype должен измеряться без нового transient detector,
чтобы отделить эффект ERB envelope от других изменений. После подтверждения
основной идеи добавляется лёгкая защита transient Main:

- spectral flux или аналогичный заранее выделенный признак;
- ослабление make-space reduction на коротких broadband attacks;
- confidence override, если Voice-to-Main conflict очень высок;
- без отдельного HPSS inverse path и без изменения latency на первом этапе.

Полная median-filter HPSS рассматривается только если простой flux protector не
сохраняет Foley и перкуссию.

### 7.5 Stereo и spatial stability

Нужно проверять:

- отсутствие прыжков image width;
- отсутствие неожиданной миграции coherent Main между L/C/R;
- согласованную обработку symmetric Voice;
- допустимую независимость L/C/R при реально панорамированном Voice;
- отсутствие artificial widening из-за несогласованных band gains.

Жёсткий L/R link DSP-масок не вводится: он может повредить корректно
панорамированный перевод. Стабильность достигается общей perceptual geometry,
ограничением slope/delta и текущим PCE.

## 8. Роль существующего weighting

Существующие `Weight Frequency`, `Weight Gain` и `Weight Q` — публичные
автоматизируемые параметры и не должны быть удалены или переименованы на уровне
ID без migration.

Во время прототипирования сравниваются три роли weighting:

1. `Classic only`: текущий bias prominence Variant A;
2. `Shared focus`: weighting меняет Target Margin make-space и одновременно
   сохраняет текущую роль в Original Bleed;
3. `Bleed only`: make-space полностью определяется Voice/Main margin, а bell
   остаётся только в Variant A.

До прослушивания предпочтительным baseline является `Bleed only`, потому что он
не смешивает две семантики. Если shared focus даст предсказуемый творческий
контроль, UI может переименовать display concept в `Focus`, сохранив parameter
IDs и state compatibility.

Weighting node, запланированная для 1.0, не должна заранее менять DSP-смысл.

## 9. Режимы прототипа и будущий parameter contract

### 9.1 До принятия алгоритма

Не добавлять новые host-visible parameters на первой исследовательской итерации.
Использовать compile-time или developer-only configuration для сравнения:

- `CLASSIC_A`;
- `MAKE_SPACE_ONLY`;
- `HYBRID_MAX`;
- при необходимости `HYBRID_BLEND`.

Это предотвращает преждевременное закрепление parameter IDs и автоматизации.

### 9.2 После успешного прослушивания

Минимальный кандидат публичного управления:

- существующий `Spectral Amount` остаётся master maximum depth;
- один strategy control `CLASSIC / MAKE SPACE / HYBRID`;
- один `Target Margin` или более музыкально названный macro control;
- существующий weighting сохраняется как Focus/Original Bleed bias;
- дополнительные expert controls не добавляются без доказанной пользы.

Для старых состояний strategy должен загружаться как `CLASSIC`, если новый
parameter отсутствует. Новый default для новых экземпляров меняется на
`HYBRID` только после успешной обратной проверки старого звучания и отдельного
продуктового решения.

Перед фиксацией layout обязательны state migration и automation tests.

## 10. Realtime, CPU, memory и latency

Новый алгоритм обязан:

- переиспользовать текущие Voice forward FFT;
- не добавлять Voice inverse FFT;
- заранее рассчитывать ERB/Bark weights при prepare/configuration;
- использовать заранее выделенные fixed/max-size buffers;
- не выделять память, не блокироваться и не писать файлы в audio thread;
- сохранять текущую FFT geometry и reported PDC;
- не менять фиксированный 50 ms Lookahead reserve;
- полностью прекращать make-space analysis при technical silence Voice после
  существующего tail contract;
- обходить лишнюю работу при нулевой make-space/overall spectral depth;
- не выполнять analysis только ради закрытого GUI;
- сохранять одинаковое realtime/offline поведение.

Начальный CPU budget определяется относительно активного spectral baseline 1.0.
До реализации нужно записать baseline по режимам QUALITY/NORMAL, FFT x1/x2/x4,
Stereo/Surround и GUI open/closed. Относительное увеличение должно быть принято
по измерениям; произвольный абсолютный процент в этом документе не фиксируется.

## 11. План реализации по этапам

### Phase 0 — immutable baseline

1. Завершить и зафиксировать релиз 1.0.
2. Сохранить characterization fixtures Variant A.
3. Записать CPU, latency, GR и output measurements.
4. Подготовить репрезентативный listening corpus.
5. Не смешивать post-1.0 sound change с рефакторингом или UI redesign.

Результат: воспроизводимая точка сравнения и возможность мгновенного fallback.

### Phase 1 — perceptual-band analysis без изменения звука

1. Добавить отдельный `PerceptualBandBank` или эквивалентный компонент.
2. Предрассчитать band/bin weights для всех FFT sizes и sample rates.
3. Считать Voice/Main band levels параллельно baseline.
4. Добавить developer snapshots/counters вне production runtime cost.
5. Проверить one-sided, dual-mono, stereo и surround fast paths.
6. Убедиться, что output Variant A не изменился.

Результат: корректные band data при bitwise/characterization unchanged output.

### Phase 2 — MAKE_SPACE_ONLY hidden prototype

1. Реализовать band presence и начальную Target Margin formula.
2. Добавить frequency interpolation и shape smoothing.
3. Применять real non-negative gains через существующий WOLA path.
4. Сохранить master Spectral Amount cap.
5. Реализовать мгновенный developer A/B с CLASSIC_A.
6. Провести artifact stress tests до добавления hybrid.

Результат: отдельно слышимый эффект make-space без влияния Variant A.

### Phase 3 — HYBRID_MAX

1. Вернуть Variant A как Original Bleed contour.
2. Объединять reductions через maximum.
3. Сравнить одинаковые total depth и Voice levels.
4. Проверить возможность уменьшить wideband depth.
5. Сравнить роли weighting из раздела 8.

Результат: кандидат, выполняющий основную цель и сохраняющий дополнительное
снижение оригинальной речи.

### Phase 4 — psychoacoustic guardrails

1. Настроить minimum bandwidth и adjacent-band slope.
2. Добавить frame delta limiter.
3. Проверить/настроить shape attack/release без двойного envelope.
4. Добавить простой transient protector.
5. Проверить spatial stability и пани́рованные Voice cases.
6. Отказаться от guardrails, которые не дают слышимой пользы.

Результат: отсутствие musical noise, talking-EQ и заметного image wobble в
принятой listening matrix.

### Phase 5 — product integration

1. Выбрать финальную strategy и defaults.
2. Утвердить минимальный parameter schema.
3. Реализовать state migration.
4. Обновить Simple/Advanced UI без перегрузки controls.
5. Обновить analyzer visualization для perceptual mask.
6. Добавить понятные tooltips: make-space не ищет оригинального актёра.
7. Обновить README, architecture, handoff и technical spec.

### Phase 6 — stabilization

1. Полный unit/integration/factory suite.
2. pluginval strictness 5 и 10.
3. REAPER realtime/offline/automation matrix.
4. Windows и macOS Debug/Release builds.
5. CPU profiling и silence optimization audit.
6. Blind listening comparison с 1.0.
7. Release только после выполнения acceptance criteria.

## 12. Тестовая стратегия

### 12.1 Unit tests perceptual bands

Проверить:

- DC/Nyquist boundaries;
- сумму и нормировку band weights;
- постоянный спектр;
- одиночный tone внутри и на границе band;
- разные sample rates и FFT x1/x2/x4;
- отсутствие NaN/Inf на silence и denormals;
- Voice band ниже presence floor даёт zero target;
- target margin, soft knee и depth cap;
- плавность соседних band gains;
- корректную интерполяцию обратно к FFT bins;
- отсутствие heap allocation в process path.

### 12.2 DSP characterization

Минимальные synthetic cases:

- Voice silence;
- mono/dual-mono Voice;
- hard-left, centre, hard-right и промежуточная панорама Voice;
- Surround L/C/R/Ls/Rs по одному и одновременно;
- разные панорамы Main content при близком расположении Voice;
- Voice и оригинальный актёр с сильно разными F0/формантами;
- pink noise, room tone и diffuse ambience;
- centred pad, bass, piano, strings и tonal effects;
- snare, hi-hat, clicks, Foley и broadband impacts;
- original dialogue, music vocals и speech-like instruments;
- scene cut и резкая смена спектра;
- automation Spectral Amount/Target Margin/strategy;
- sleep → wake → active → tail → sleep.

Разная панорама оригинала и перевода вне небольшой рабочей погрешности не
является acceptance case. Такой материал можно использовать только как
diagnostic stress test, а не как основание добавлять auto-tracking.

### 12.3 Latency и reconstruction

Для каждого mode/sample rate/FFT size:

- measured latency равна reported latency;
- Lookahead reserve остаётся 50 ms;
- low/high/LFE остаются выровненными;
- inactive/zero-depth path сохраняет baseline reconstruction;
- strategy switch следует принятому stopped-only/crossfade contract;
- PDC не меняется из-за spectral strategy;
- realtime и offline output совпадают в пределах tolerance.

### 12.4 State и compatibility

- состояние 1.0 без новых parameters загружается как CLASSIC;
- round-trip новых parameters стабилен;
- старые Weight IDs сохраняют значения;
- choice ordering не меняется без migration;
- automation не вызывает discontinuity;
- pending DSP configuration не теряет strategy state.

## 13. Listening и measurement corpus

### 13.1 Обязательные материалы

Нужны короткие размеченные фрагменты:

- тихая и громкая мужская речь;
- тихая и громкая женская речь;
- шёпот, согласные, дыхание и эмоциональная речь;
- translation Voice, намеренно отличающийся от оригинального актёра;
- stereo и 5.1;
- центрированная и умеренно панорамированная речь;
- ambience-only;
- dialogue над ambience;
- dialogue над tonal music;
- dialogue над percussion/Foley;
- плотный full mix;
- сцены с заметным reverb, но без требования его автоматического копирования.

Если доступны исходные dialogue/M&E stems, их следует использовать только как
ground truth для разработки и измерений. Runtime-плагин не должен требовать эти
stems.

### 13.2 Что слушать

Всегда сравнивать при loudness-matched Voice:

1. unprocessed Main + Voice;
2. 1.0 CLASSIC + Voice;
3. MAKE_SPACE_ONLY + Voice;
4. HYBRID_MAX + Voice;
5. solo Processed Main;
6. Delta/removed content.

Основные субъективные вопросы:

- Voice стал понятнее без ненужного повышения громкости?
- музыка и ambience сохраняют непрерывность?
- слышен talking EQ или spectral warble?
- исчезают ли transient attacks?
- меняется ли стереообраз?
- не становится ли сцена неоправданно «глухой»?
- Original Bleed действительно помогает или только повреждает фон?
- можно ли уменьшить wideband depth относительно 1.0?

### 13.3 Измерения

Использовать как вспомогательные, а не единственные критерии:

- Voice-to-Processed-Main contrast по perceptual bands;
- integrated и short-term Voice/background level difference;
- изменение Main spectral envelope;
- stereo correlation и width;
- transient crest/flux loss;
- residual original-dialogue projection при наличии ground truth;
- M&E distortion при наличии ground truth;
- active/silent CPU, allocations и worst-frame time;
- measured PDC и offline/realtime null.

Перед сравнением defaults все варианты должны иметь сопоставимую максимальную
depth и loudness, иначе listening result будет отражать уровень, а не алгоритм.

## 14. Acceptance criteria

Post-1.0 enhancement принимается только если одновременно выполнено:

1. В blind listening итоговая сумма с Voice не хуже 1.0 по разборчивости ни на
   одном обязательном классе материала и лучше на значимой части corpus.
2. Music/ambience preservation субъективно лучше или равно 1.0 при одинаковой
   разборчивости Voice.
3. Нет устойчивого musical noise, talking-EQ, moving-notch или image wobble.
4. Foley/percussion не повреждаются сильнее 1.0 без компенсирующего выигрыша.
5. Разные актёры Voice/Main не требуют spectral correspondence.
6. Корректно работают Stereo/Surround, QUALITY/NORMAL и FFT x1/x2/x4.
7. Silence optimization, realtime safety, PDC и state compatibility сохранены.
8. CPU increase измерен, объяснён и принят относительно baseline.
9. Старые проекты воспроизводят CLASSIC semantics после загрузки.
10. Документация ясно разделяет make-space и original-dialogue removal.

Если MAKE_SPACE_ONLY не превосходит Classic по главной продуктовой цели, режим
не добавляется в публичный UI. Наличие реализованного прототипа само по себе не
является основанием для shipping.

## 15. Риски и способы ограничения

| Риск | Причина | Ограничение |
|---|---|---|
| Talking EQ | точная Voice harmonic mask | ERB/cepstral envelope, minimum width |
| Musical noise | независимые узкие gains | band interpolation, slope/delta limits |
| Двойной attack/release | два каскадных envelopes | shape smoothing до общей Voice envelope |
| Избыточный duck при пустом band | формула Main − Voice | обязательный band presence gate |
| Неверная оценка margin | sidechain не совпадает с audible Voice | post-fader routing или calibration |
| Потеря Foley | broadband Voice/transient overlap | transient protector и depth cap |
| Потеря coherent music | одинаковая spatial/frequency область | minimum required margin, hybrid tuning |
| Stereo wobble | независимые channel masks | общая geometry, temporal/slope constraints |
| Рост CPU | дополнительные band accumulations | reuse FFT, precomputed weights, sleep/bypass |
| Поломка старых проектов | новый parameter schema | CLASSIC fallback и migration tests |
| Scope creep в source separation | попытка найти актёра в Main | держать make-space objective и non-goals |

## 16. Decision log

Следующие решения считаются принятыми и не требуют повторного расследования без
новых результатов прослушивания:

1. Конечная цель — разборчивость перевода при сохранении атмосферы, а не
   максимальное удаление оригинального актёра.
2. Sound engineer отвечает за близкую панораму Voice и оригинала.
3. Случай заметно различающейся панорамы исключён из product scope.
4. Auto spatial tracking оригинального актёра не нужен.
5. Sound engineer отвечает за reverb/delay и художественное пространство Voice.
6. Разная длина реплик и короткие release traces — допустимые nice-to-have
   проблемы.
7. Ambience refill не является базовой стадией: сначала нужно минимизировать
   collateral reduction.
8. Voice spectrum нельзя использовать как карту частот оригинального актёра.
9. Voice spectrum можно использовать как карту headroom, необходимого самому
   переводному Voice.
10. Точная FFT-bin Voice mask отвергнута в пользу perceptually smoothed envelope.
11. Текущий weighting — статический упрощённый аналог make-space bias.
12. Variant A сохраняется как Original Bleed и fallback.
13. Первый hybrid candidate объединяет contours через maximum.
14. Новые host parameters не фиксируются до успешного hidden prototype.
15. Финальная оценка всегда проводится на `Processed Main + audible Voice`.

## 17. Открытые вопросы для экспериментов

Эти вопросы не следует решать повторным общим обсуждением; для каждого нужен
контролируемый A/B prototype:

1. 24 или 32 perceptual bands?
2. ERB/Bark или cepstral envelope лучше сохраняет музыку?
3. Какая band presence curve не пропускает слабые согласные Voice?
4. Нужен flat Target Margin или speech-importance curve?
5. Какой margin даёт лучший trade-off clarity/background preservation?
6. Достаточен `HYBRID_MAX` или нужен weighted blend?
7. Должен weighting влиять только на Original Bleed или также на make-space?
8. Нужен ли отдельный transient protector после band smoothing?
9. Можно ли уменьшить wideband depth и сохранить разборчивость?
10. Нужен ли публичный strategy switch или HYBRID может стать единственным
    новым default для новых instances?

Ответ по каждому пункту фиксируется в этом документе или следующем architecture
decision record вместе с corpus, настройками, измерениями и результатом
прослушивания.

## 18. Рекомендуемая точка старта после 1.0

Первый post-1.0 coding increment должен быть намеренно маленьким:

1. добавить characterization tests текущего Variant A;
2. реализовать заранее рассчитанный ERB band accumulator;
3. собирать Voice/Main band levels без изменения output;
4. подтвердить отсутствие дополнительных allocations и измерить изменение CPU;
5. сохранить output baseline неизменным;
6. только после этого добавить скрытый MAKE_SPACE_ONLY gain path.

Не начинать с UI, новых host parameters, ambience reconstruction, auto dialogue
detection или рефакторинга всего `SpectralSuppressor.h`. Сначала должен быть
доказан слышимый выигрыш минимального perceptual make-space алгоритма.
