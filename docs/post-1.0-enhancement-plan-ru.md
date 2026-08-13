# Smart Ducking — post-1.0 enhancement plan

Статус документа: согласованный подробный план исследований и улучшений после
стабильного релиза `1.0`.

Дата фиксации: 11 августа 2026 года.

Этот документ не изменяет scope текущего `1.0.0-rc.2`. Этапы A–D и RC2 CPU/offline
optimization из [`implementation-plan-ru.md`](implementation-plan-ru.md) завершены;
до финализации и продуктовой приёмки `1.0` приоритет имеют текущий Variant A и
существующие DSP-контракты. Работы из этого документа начинаются только от
измеренного, прослушанного и зафиксированного baseline `1.0`.

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
- подбор room/reverb/delay для переводного Voice, если пространство сцены или
  художественный замысел требуют совпадения с оригиналом;
- решение, в каких сценах автоматизировать опциональный `Room Tail Suppression`:
  алгоритм может обнаружить хвост, связанный с оригинальной репликой, но не
  способен определить, является ли он технической акустикой помещения или
  намеренным художественным эффектом;
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
- автоматическое определение художественного намерения reverb/delay;
- WPE/MCLP, оценку длинного impulse response, адаптивное фазовое вычитание и
  другие тяжёлые dereverberation-контуры;
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

### 4.3 Technical Room Tail Reduction — дополнительный опциональный контур

Этот контур предназначен для специального, но практически важного случая:

- сухой оригинальный голос присутствует преимущественно в одном direct spatial
  component Main;
- в L/R/Ls/Rs или других соседних компонентах находится заметная поздняя
  реверберация этого голоса;
- в целевом компоненте почти нет сухого голоса, но одновременно могут звучать
  музыка, ambience и эффекты, которые необходимо сохранить.

Первый и утверждённый подход — лёгкий delayed band-envelope predictor. Он
связывает огибающие direct-компонента Main с задержанными затухающими
огибающими целевых компонентов и формирует мягкую time-frequency маску
предполагаемого хвоста. Точное фазовое вычитание и восстановление room impulse
response не выполняются.

Voice используется только как activity/spatial gate и не является спектральным
эталоном оригинального актёра. Reference для хвоста берётся из необработанного
`Main` до spectral/wideband gain. Подробный DSP-контракт приведён в разделе 8.

Режим не пытается автоматически отличить акустику пещеры или ангара от
художественного «голоса бога». Поэтому он должен быть отключаемым,
автоматизируемым по сценам и иметь безопасный zero/off default для старых
instances. Добавление diffuseness/CDR допускается только как отдельный
эксперимент и не является утверждённой стадией реализации.

### 4.4 Hybrid

Начальный кандидат объединения — максимум в dB, а не сумма:

```text
FinalSpectralReduction(f, t, channel) = max(
    MakeSpaceReduction(f, t, channel),
    OriginalBleedReduction(f, t, channel),
    RoomTailReduction(f, t, channel))
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

Main direct component before gains
  → perceptual-band envelopes
  → delayed decay predictor
Main spatial target components before gains
  → delayed envelope relation/confidence
  → Room Tail Reduction

max(Make-Space, Original Bleed, Room Tail)
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

## 8. Technical Room Tail Reduction

### 8.1 Цель и границы режима

Для perceptual band `b`, выбранного direct-компонента Main и целевого spatial
component `j` используется модель мощности:

```text
TargetPower[j, b, n] = BackgroundPower[j, b, n]
                     + LateReverbPower[j, b, n]

LateReverbPower[j, b, n] ≈ decayFilter[j, b](DirectPower[b, n - delay])
```

Алгоритм оценивает второе слагаемое и уменьшает его вклад мягким gain, но не
пытается восстановить изолированный M&E. Если в том же time-frequency region
одновременно находится ambience, часть его энергии неизбежно попадает под
общий gain; обязательны confidence, depth cap и плавное ограничение.

Ожидаемый основной случай:

- в 5.1 сухая речь находится преимущественно в C или другом явно заданном
  direct-компоненте;
- поздние отражения находятся в L/R/Ls/Rs;
- в целевом компоненте отсутствует или почти отсутствует синхронный сухой
  голос;
- direct reference остаётся доступен в другом компоненте необработанного Main.

Отсутствие сухого голоса в целевом канале помогает разделению. Отсутствие
сухого оригинального голоса во всех компонентах Main, напротив, делает
привязку хвоста к конкретной реплике неопределённой; в таком случае режим
должен безопасно давать нулевое подавление.

В Stereo/PCE тот же подход допустим, но confidence и maximum depth должны быть
консервативнее: музыка и ambience чаще делят L/R с предполагаемым хвостом, а
разделение direct/diffuse менее определённо, чем в 5.1.

### 8.2 Reference и пространственный контракт

Reference рассчитывается по `Main` до применения любых текущих spectral и
wideband gains. Обработанный сигнал нельзя подавать обратно в estimator: иначе
возникнет feedback, при котором сделанное подавление меняет оценку следующего
кадра.

Существующие spatial weights Voice используются только для:

1. открытия детектора во время переводной реплики;
2. выбора соответствующего direct-компонента Main согласно принятому контракту
   близкой панорамы;
3. определения допустимых соседних/окружающих target-компонентов.

Voice FFT bins, F0 и форманты не участвуют в оценке хвоста. Последние валидные
spatial weights и история direct envelope удерживаются на время анализа
реверберационного хвоста, поэтому детектор не выключается сразу после окончания
Voice. Отдельный tracker панорамы оригинального актёра не добавляется.

LFE исключён из reference, анализа и подавления. Target с заметным синхронным
dry-like вкладом должен получать пониженный confidence: этот режим рассчитан на
компоненты, где доминируют поздние отражения, а не на удаление прямой речи.

### 8.3 Утверждённый delayed band-envelope predictor

Первая реализация должна переиспользовать тот же `PerceptualBandBank`, который
добавляется для make-space, и не выполнять новые inverse FFT. Для каждой
perceptual band сохраняются power envelopes direct reference и target spatial
components.

Начальный predictor — небольшой банк фиксированных delayed decay states:

```text
tailCandidate[m, b, n] =
    alpha[m] * tailCandidate[m, b, n - 1]
  + (1 - alpha[m]) * DirectPower[b, n - delay[m]]

estimatedReverb[j, b, n] =
    scale[j, b] * combineStableCandidates(tailCandidate[:, b, n])
```

`m` представляет несколько характерных pre-delay/decay вариантов. Стартовый
offline search должен охватить короткие комнаты и длинные хвосты порядка
нескольких секунд; конкретные delay, T60-grid и количество states не являются
публичным DSP-контрактом до benchmark. Банк должен оставаться малым и работать
на band rate, а не как длинный sample-rate FIR.

`scale` оценивается медленно и ограниченно только на кадрах с валидным direct
reference. Он не должен свободно обучаться на ambience-only, sustained music,
scene cut или technical silence. Переключение лучшего decay candidate не должно
происходить покадрово: нужны hysteresis или плавная confidence-weighted смесь,
чтобы избежать pumping.

Базовый confidence формируется без CDR из следующих признаков:

- существующая активность Voice как trigger первоначальной связи;
- достаточный уровень и стабильность direct reference Main;
- задержанная корреляция band envelopes direct и target;
- соответствие target envelope одному из плавно затухающих candidates;
- отсутствие резкого scene cut;
- spectral continuity между соседними perceptual bands;
- отсутствие достаточной evidence для связи даёт zero reduction.

Это energy-domain prediction. Фаза target не оценивается, reverb waveform не
вычитается, а decorrelated/modulated stereo tail допустим до тех пор, пока его
полосовая энергия сохраняет задержанную связь с direct reference.

### 8.4 Формирование gain и совместная работа контуров

Начальный gain — Wiener-подобная оценка остаточной мощности с консервативным
коэффициентом `beta`:

```text
removablePower = beta * confidence * estimatedReverb
remainingPower = max(TargetPower - removablePower,
                     minimumRemainingRatio * TargetPower)
roomTailGain   = sqrt(remainingPower / max(TargetPower, epsilon))
```

Обязательны:

- общий maximum depth, управляемый `Room Tail Suppression`;
- soft knee по confidence и estimated reverb-to-target ratio;
- perceptual-band interpolation и существующие slope/delta constraints;
- отсутствие boost выше unity;
- finite guards и безопасный silence path;
- real non-negative gain через существующий WOLA path;
- мягкий reduction вместо попытки полного null хвоста.

Room Tail contour объединяется с Make-Space и Original Bleed через maximum в
dB, а не суммой reductions. У него есть собственное состояние хвоста: после
исчезновения текущей Voice envelope он может продолжать подавление найденной
реверберации. При этом отдельный второй attack/release для обычного make-space
не вводится.

Scene cut, seek, prepare/reset, bus-layout change и discontinuity обязаны
очищать decay/history/scale state. При смене панорамы новая реплика не должна
наследовать spatial reference предыдущей.

### 8.5 Realtime, tail и sleep contract

Режим обязан:

- работать на уже рассчитанных band powers;
- хранить fixed-size state без allocation в audio thread;
- не добавлять latency и не менять PDC/Lookahead reserve;
- иметь одинаковое realtime/offline поведение;
- при нулевом control полностью обходить predictor и сохранять baseline path;
- продолжать работу после Voice только пока estimated tail/confidence выше
  технического floor и не превышен защитный maximum tail interval;
- переходить в sleep после завершения tail, а не удерживать processor активным
  бесконечно из-за шума помещения;
- не обновлять `scale` на собственном уже обработанном output.

Банк decay states и maximum tail interval должны корректно пересчитываться из
миллисекунд/секунд в frame count для всех sample rates и FFT x1/x2/x4.

### 8.6 Parameter и product contract

Первая итерация реализуется как hidden/developer mode без host-visible
parameter. После успешного прослушивания минимальный публичный контракт — один
автоматизируемый macro control `Room Tail Suppression`:

- `0` полностью отключает analysis и gain;
- увеличение значения одновременно разрешает большую уверенность/глубину, но
  не отключает safety cap;
- старые states и instances загружаются со значением `0`;
- управление предназначено для автоматизации по сценам;
- отдельные публичные T60, pre-delay, diffuseness и adaptation controls не
  добавляются без доказанной необходимости.

Режим подавляет reverb оригинальной реплики, но не создаёт reverb переводного
Voice. Подбор пространства Voice остаётся задачей звукорежиссёра.

### 8.7 Неутверждённый эксперимент: diffuseness/CDR confidence

После готового envelope-only baseline допускается отдельный A/B experiment:
добавить межканальную coherence/diffuseness или CDR-подобную оценку только как
множитель confidence. Гипотеза: хвост речи чаще выглядит diffuse, а синхронный
direct content — coherent.

Эксперимент не считается частью утверждённого плана реализации, потому что:

- ambience и широкая музыка также могут быть diffuse;
- каналы готового Stereo/5.1 mix не являются физическим microphone array;
- synthetic reverb, decorrelation и mastering нарушают стандартные CDR-модели;
- дополнительная матричная/ковариационная оценка увеличивает CPU и сложность.

Он принимается только если на одинаковом envelope predictor и depth статистически
снижает false positive reduction ambience/music, не пропускает слышимые room
tails и укладывается в CPU budget. Отсутствие выигрыша означает полное удаление
CDR-ветки и никак не блокирует shipping envelope-only режима.

## 9. Роль существующего weighting

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

## 10. Режимы прототипа и будущий parameter contract

### 10.1 До принятия алгоритма

Не добавлять новые host-visible parameters на первой исследовательской итерации.
Использовать compile-time или developer-only configuration для сравнения:

- `CLASSIC_A`;
- `MAKE_SPACE_ONLY`;
- `HYBRID_MAX`;
- при необходимости `HYBRID_BLEND`.

После фиксации основного hybrid baseline отдельно добавляется
`ROOM_TAIL_ENVELOPE`. Он не должен смешиваться с первым make-space A/B, иначе
будет невозможно определить источник слышимого изменения. CDR-вариант не
входит в обязательный список prototype modes.

Это предотвращает преждевременное закрепление parameter IDs и автоматизации.

### 10.2 После успешного прослушивания

Минимальный кандидат публичного управления:

- существующий `Spectral Amount` остаётся master maximum depth;
- один strategy control `CLASSIC / MAKE SPACE / HYBRID`;
- один `Target Margin` или более музыкально названный macro control;
- существующий weighting сохраняется как Focus/Original Bleed bias;
- после отдельного успешного Room Tail prototype — один автоматизируемый
  `Room Tail Suppression` с zero/off default для старых states;
- дополнительные expert controls не добавляются без доказанной пользы.

Для старых состояний strategy должен загружаться как `CLASSIC`, если новый
parameter отсутствует. Новый default для новых экземпляров меняется на
`HYBRID` только после успешной обратной проверки старого звучания и отдельного
продуктового решения.

Перед фиксацией layout обязательны state migration и automation tests.

### 10.3 Независимые временные параметры Wideband и Spectral

После стабилизации основного make-space алгоритма общие временные параметры
нужно разделить на два набора:

- `Wideband Attack / Hold / Release / Lookahead`;
- `Spectral Attack / Hold / Release / Lookahead`.

Это разделение должно позволять независимо настраивать реакцию обычной и
спектральной динамики: например, быстро освобождать спектральное место для
Voice, но сохранять более плавное широкополосное приглушение. Оба набора
остаются host-visible и автоматизируемыми.

Рядом с ними нужен опциональный `Link` одноимённых параметров. При включённом
link изменение Attack меняет оба Attack, изменение Hold — оба Hold и так далее;
связь между разными типами параметров не создаётся. При выключении link каждый
набор редактируется независимо и сохраняет последние связанные значения как
начальную точку.

Контракт совместимости:

- старые states с одним набором времени загружают эти значения одновременно в
  Wideband и Spectral и включают link;
- новый экземпляр по умолчанию также стартует с link, сохраняя поведение и
  простоту настройки версии 1.0;
- link является UI/parameter-edit contract и не должен в audio thread постоянно
  перезаписывать автоматизацию одного набора значениями другого;
- независимые Lookahead не должны добавлять два последовательных delay:
  reported PDC определяется максимальным активным Lookahead, а более короткий
  контур получает соответствующее внутреннее выравнивание;
- диапазоны Lookahead должны оставаться внутри утверждённого latency reserve,
  либо изменение этого резерва принимается отдельным решением с PDC, state и
  automation tests.

Перед публичным включением обязательны тесты загрузки старых states,
link/unlink, автоматизации обоих наборов, latency alignment и отсутствие
двойного audible envelope при одновременной работе Wideband и Spectral.

## 11. Realtime, CPU, memory и latency

Новый алгоритм обязан:

- переиспользовать текущие Voice forward FFT;
- не добавлять Voice inverse FFT;
- заранее рассчитывать ERB/Bark weights при prepare/configuration;
- использовать заранее выделенные fixed/max-size buffers;
- не выделять память, не блокироваться и не писать файлы в audio thread;
- сохранять текущую FFT geometry и reported PDC;
- не менять фиксированный 50 ms Lookahead reserve;
- полностью прекращать make-space analysis при technical silence Voice после
  существующего tail contract; Room Tail analysis является отдельным
  исключением и может продолжаться только по правилам раздела 8.5;
- обходить лишнюю работу при нулевой make-space/overall spectral depth;
- полностью обходить decay predictor при нулевом `Room Tail Suppression`;
- не выполнять analysis только ради закрытого GUI;
- сохранять одинаковое realtime/offline поведение.

Начальный CPU budget определяется относительно активного spectral baseline 1.0.
До реализации нужно записать baseline по режимам QUALITY/NORMAL, FFT x1/x2/x4,
Stereo/Surround и GUI open/closed. Относительное увеличение должно быть принято
по измерениям; произвольный абсолютный процент в этом документе не фиксируется.

## 12. План реализации по этапам

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
5. Сравнить роли weighting из раздела 9.

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
3. Разделить Attack/Hold/Release/Lookahead на Wideband и Spectral и добавить
   опциональный link по правилам раздела 10.3.
4. Реализовать state migration, включая перенос общего набора времени 1.0 в
   оба новых набора с включённым link.
5. Расширить текущие Dynamics/Routing pages без возврата удалённой иерархии и без перегрузки controls.
6. Обновить analyzer visualization для perceptual mask.
7. Добавить понятные tooltips: make-space не ищет оригинального актёра.
8. Обновить README, architecture, handoff и technical spec.

### Phase 6 — stabilization

1. Полный unit/integration/factory suite.
2. pluginval strictness 5 и 10.
3. REAPER realtime/offline/automation matrix.
4. Windows и macOS Debug/Release builds.
5. CPU profiling и silence optimization audit.
6. Blind listening comparison с 1.0.
7. Release основного make-space только после выполнения core acceptance
   criteria из раздела 15.1.

### Phase 7 — ROOM_TAIL_ENVELOPE после основного make-space

1. Зафиксировать hybrid baseline и не менять одновременно его tuning.
2. Добавить сбор pre-gain direct/target band envelopes без изменения output.
3. Реализовать малый банк delayed decay states из раздела 8.3.
4. Добавить envelope relation confidence, scale freeze и scene-cut reset.
5. Сформировать мягкий Room Tail gain и объединить contours через maximum.
6. Реализовать независимый tail/sleep contract и проверку seek/reset.
7. Провести developer A/B на размеченном Stereo/5.1 corpus с wet-only targets.
8. Если acceptance criteria из раздела 15.2 выполнены, добавить один
   автоматизируемый macro control и state migration с zero default.
9. Не добавлять diffuseness/CDR в production path в рамках этой фазы.

Результат: лёгкое опциональное подавление технического room tail оригинальной
реплики без WPE/MCLP, длинного adaptive FIR и фазового вычитания. Неудача
последующего CDR-эксперимента не влияет на статус этой фазы.

## 13. Тестовая стратегия

### 13.1 Unit tests perceptual bands

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

### 13.2 DSP characterization

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

### 13.3 Latency и reconstruction

Для каждого mode/sample rate/FFT size:

- measured latency равна reported latency;
- Lookahead reserve остаётся 50 ms;
- low/high/LFE остаются выровненными;
- inactive/zero-depth path сохраняет baseline reconstruction;
- strategy switch следует принятому stopped-only/crossfade contract;
- PDC не меняется из-за spectral strategy;
- realtime и offline output совпадают в пределах tolerance.

### 13.4 State и compatibility

- состояние 1.0 без новых parameters загружается как CLASSIC;
- round-trip новых parameters стабилен;
- старые Weight IDs сохраняют значения;
- choice ordering не меняется без migration;
- automation не вызывает discontinuity;
- pending DSP configuration не теряет strategy state;
- состояние без `Room Tail Suppression` загружается со значением `0`;
- round-trip и automation Room Tail control не оставляют активный decay state
  после reset/off.

### 13.5 Room Tail tests

Для Room Tail нужен отдельный synthetic и stem-based набор, где известны dry
reference, добавленный wet tail и независимый background. Минимальная matrix:

- direct speech в C, wet-only копия через известный room IR в L/R/Ls/Rs;
- разные pre-delay и frequency-dependent T60 от короткой комнаты до нескольких
  секунд;
- decorrelated stereo/surround reverb и умеренно модулированный tail;
- речь заканчивается, а хвост продолжается при Voice silence;
- ambience-only и music-only без direct reference дают zero reduction;
- dry reference без reverb target даёт zero reduction;
- reverb target одновременно с room tone, wind, crowd, tonal music и Foley;
- сильный dry-like сигнал в target снижает confidence;
- две близкие реплики с разными spatial weights не смешивают history;
- scene cut, transport seek, reset и sample-rate/FFT change очищают history;
- автоматизация `Room Tail Suppression` не создаёт click и не оставляет stale
  gain;
- tail завершается и processor возвращается в sleep;
- Stereo/PCE работает консервативнее, Surround не затрагивает LFE;
- realtime/offline output совпадает в пределах tolerance;
- zero control сохраняет baseline output и CPU fast path.

Измерять отдельно attenuation известного wet stem и повреждение независимого
background stem. Общий спад уровня target channel не считается доказательством
качества без этих двух раздельных метрик.

## 14. Listening и measurement corpus

### 14.1 Обязательные материалы

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
- сцены с заметным техническим room reverb: пещера, пустой ангар, большой зал;
- художественные reverb/delay cases, на которых Room Tail должен быть вручную
  отключён;
- раздельные direct/wet/background stems хотя бы для части Room Tail corpus.

Если доступны исходные dialogue/M&E stems, их следует использовать только как
ground truth для разработки и измерений. Runtime-плагин не должен требовать эти
stems.

### 14.2 Что слушать

Всегда сравнивать при loudness-matched Voice:

1. unprocessed Main + Voice;
2. 1.0 CLASSIC + Voice;
3. MAKE_SPACE_ONLY + Voice;
4. HYBRID_MAX + Voice;
5. HYBRID_MAX + ROOM_TAIL_ENVELOPE + Voice на room-tail material;
6. solo Processed Main;
7. Delta/removed content.

Основные субъективные вопросы:

- Voice стал понятнее без ненужного повышения громкости?
- музыка и ambience сохраняют непрерывность?
- слышен talking EQ или spectral warble?
- исчезают ли transient attacks?
- меняется ли стереообраз?
- не становится ли сцена неоправданно «глухой»?
- Original Bleed действительно помогает или только повреждает фон?
- можно ли уменьшить wideband depth относительно 1.0?
- уменьшается ли слышимый хвост оригинального актёра после окончания слова?
- остаются ли room tone, crowd, wind и diffuse music непрерывными?
- не создаёт ли Room Tail pumping на длинных decay и соседних репликах?

### 14.3 Измерения

Использовать как вспомогательные, а не единственные критерии:

- Voice-to-Processed-Main contrast по perceptual bands;
- integrated и short-term Voice/background level difference;
- изменение Main spectral envelope;
- stereo correlation и width;
- transient crest/flux loss;
- residual original-dialogue projection при наличии ground truth;
- attenuation известного wet-only reverb stem после окончания direct speech;
- false-positive attenuation независимого ambience/music stem при активном
  Room Tail;
- delayed envelope confidence на true-tail и ambience-only segments;
- M&E distortion при наличии ground truth;
- active/silent CPU, allocations и worst-frame time;
- measured PDC и offline/realtime null.

Перед сравнением defaults все варианты должны иметь сопоставимую максимальную
depth и loudness, иначе listening result будет отражать уровень, а не алгоритм.

## 15. Acceptance criteria

### 15.1 Core Make-Space/Original Bleed

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

### 15.2 Technical Room Tail Reduction

Room Tail принимается отдельно и не блокирует release основного make-space.
Для его shipping должны одновременно выполняться условия:

1. Envelope-only Room Tail не активируется без надёжной delayed relation с
   direct reference Main.
2. На размеченных wet-only cases он слышимо и измеримо уменьшает хвост без
   WPE/MCLP или phase cancellation.
3. Повреждение независимых ambience/music/Foley stems не превышает принятый
   listening/measurement budget при сопоставимой глубине подавления хвоста.
4. Zero Room Tail control сохраняет baseline output, PDC и fast path, а старые
   states загружаются с режимом off.
5. Tail state корректно завершается, переживает automation/transport reset без
   stale gain и возвращает processor в sleep.

Если эти критерии не выполнены, `ROOM_TAIL_ENVELOPE` остаётся hidden prototype
или удаляется. Это не отменяет принятый HYBRID_MAX и не является основанием
добавлять более тяжёлый dereverberation-контур.

## 16. Риски и способы ограничения

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
| Room Tail подавляет ambience | diffuse background совпал с decay envelope | delayed relation confidence, depth cap, soft gain |
| Room Tail обучился на музыке | direct reference загрязнён Main content | Voice trigger, slow bounded scale, freeze/reset |
| Хвост обрывается или pumping | нестабильный выбор decay candidate | малый decay bank, hysteresis/blend, frame delta limit |
| Художественный reverb удалён | намерение невозможно определить по сигналу | opt-in control и automation звукорежиссёра |
| Processor не засыпает | noise поддерживает слабый tail state | confidence floor и maximum tail interval |
| Поломка старых проектов | новый parameter schema | CLASSIC fallback и migration tests |
| Scope creep в source separation | попытка найти актёра в Main | держать make-space objective и non-goals |

## 17. Decision log

Следующие решения считаются принятыми и не требуют повторного расследования без
новых результатов прослушивания:

1. Конечная цель — разборчивость перевода при сохранении атмосферы, а не
   максимальное удаление оригинального актёра.
2. Sound engineer отвечает за близкую панораму Voice и оригинала.
3. Случай заметно различающейся панорамы исключён из product scope.
4. Auto spatial tracking оригинального актёра не нужен.
5. Sound engineer создаёт reverb/delay переводного Voice и решает, в каких
   сценах включать подавление room tail оригинала.
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
16. Technical Room Tail Reduction принят как отдельный лёгкий post-make-space
    режим на delayed perceptual-band envelopes Main.
17. Room Tail reference берётся из pre-gain Main; Voice используется только как
    activity/spatial gate, но не как спектральный образец оригинального актёра.
18. WPE/MCLP, длинный adaptive impulse-response estimator и phase cancellation
    не входят в утверждённую реализацию.
19. Алгоритм не классифицирует технический и художественный reverb; режим
    является opt-in и автоматизируется звукорежиссёром.
20. Diffuseness/CDR не является утверждённым шагом и допускается только как
    изолированный эксперимент после envelope-only baseline.

## 18. Открытые вопросы для экспериментов

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
11. Какой малый набор pre-delay/T60 candidates покрывает corpus без заметного
    роста CPU?
12. Какая delayed-envelope confidence curve лучше отделяет room tail от room
    tone, sustained music и crowd?
13. Какой maximum tail interval не обрывает длинные помещения и гарантирует
    возврат в sleep?
14. Достаточен ли один `Room Tail Suppression`, или автоматизация требует
    отдельного control чувствительности? Второй control не добавлять без A/B.
15. Экспериментально: уменьшает ли diffuseness/CDR multiplier false positives
    относительно envelope-only baseline на готовых Stereo/5.1 mixes?

Ответ по каждому пункту фиксируется в этом документе или следующем architecture
decision record вместе с corpus, настройками, измерениями и результатом
прослушивания.

## 19. Рекомендуемая точка старта после 1.0

Первый post-1.0 coding increment должен быть намеренно маленьким:

1. добавить characterization tests текущего Variant A;
2. реализовать заранее рассчитанный ERB band accumulator;
3. собирать Voice/Main band levels без изменения output;
4. подтвердить отсутствие дополнительных allocations и измерить изменение CPU;
5. сохранить output baseline неизменным;
6. только после этого добавить скрытый MAKE_SPACE_ONLY gain path.

Room Tail не входит в первый coding increment. Он начинается отдельной Phase 7
после того, как HYBRID_MAX и perceptual-band infrastructure стабилизированы, и
переиспользует уже проверенный band accumulator.

Не начинать с UI, новых host parameters, ambience reconstruction, auto dialogue
detection или рефакторинга всего `SpectralSuppressor.h`. Сначала должен быть
доказан слышимый выигрыш минимального perceptual make-space алгоритма.
