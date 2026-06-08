# LARGE_PARTICIPANTS_AND_INEFFICIENCIES

Формализация ручных понятий «крупный участник» и «неэффективность рынка» из
`docs/крупные участники.docx` и `docs/неэфективности.docx`. Документ является
traceability-слоем между DOCX-источниками, существующими сигналами
`SIGNAL_DETECTION.md`, стратегиями `STRATEGIES.md`, контрактом MetaScalp API
`METASCALP_API_CONTRACT.md` и будущими тикетами реализации.

Цель: отделить формально проверяемые offline-паттерны от live-only/ambiguous
наблюдений и не допустить внедрения «ручных догадок» в live-код без данных,
явного instrument identity и contract tests.

---

## 1. Источники и API-границы

### 1.1. Ручные источники

| Источник | Покрытые понятия |
|----------|------------------|
| `docs/крупные участники.docx` | плотность как участник, завал плотностей, отскок/пробой от плотности, вход на фьюче от спот-плотности, истинность плотности, настройки крупного объёма |
| `docs/неэфективности.docx` | определение эффективности рынка, переставление плотности, реализация объёма, разжатая пружина, повторные прострелы/ликвидации, раскорреляция, раздача, роботы |
| `STRATEGY_SOURCE_DIGEST.md` | нормализованная выжимка ручных источников; используется как краткий индекс, но не заменяет DOCX для деталей |

### 1.2. Разрешённые MetaScalp данные

Используем только поля и события из `METASCALP_API_CONTRACT.md`:

| Наблюдение | Документированная опора MetaScalp | Ограничение |
|------------|-----------------------------------|-------------|
| Крупная лимитная заявка/плотность | `orderbook_snapshot`, `orderbook_update`: `Asks`, `Bids`, `Updates[].{Price, Size, Type}`, `BestAsk`, `BestBid` | Нет order-id на уровне стакана; «та же заявка» определяется эвристикой размера/стороны/траектории, не доказуемо |
| Разъедание плотности | `trade_update`: `Trades[].{Price, Size, Side, Time}` + order book deltas | `trade_update` может быть агрегирован через `AddingTicksForAPeriod`; raw tape требует настройки `0` или отдельной калибровки |
| Большой тик/крупная заявка как приоритет тикера | `notification_subscribe`: `BigTick`, `BigOrderBookAmount`, `BigOrderBookAmount2`, поля `ExchangeId`, `MarketType`, `Ticker`, `Price`, `Size`, `Date` | Нет `ConnectionId`; фильтрация только по документированным `ExchangeId/MarketType/Ticker` |
| Операторский порог крупного объёма | `GET /api/connections/{ConnectionId}/orderbook-settings?Ticker=...`: `LargeAmountUsd`, `LargeAmountUsd2` | Read-only default; live-update настроек не требуется для сигналов |
| Кластер/объёмный профиль | `GET /cluster-snapshot?Ticker&TimeFrame&ZoomIndex` | Rate limits не документированы; использовать conservative polling |
| Funding/mark | `funding_update`, `mark_price_update` | Доступность зависит от market/exchange; отсутствие after subscribe — valid live gate |

Запрещено:

- выводить spot/futures/perp identity из строковых эвристик `BTCUSDT`, `BTC_USDT`, `PERP`;
- считать `orderbook_update` доказательством одной и той же биржевой заявки;
- добавлять неописанные поля MetaScalp API без live contract test;
- торговать live-паттерны с внешними фидами, если feed stale или mapping не задан.

---

## 2. Формальная модель «крупного участника»

### 2.1. Определение

**Крупный участник** в рамках доступных данных — не идентифицируемое лицо/аккаунт,
а наблюдаемая серия действий, совместимая с участником, которому нужно
разместить или реализовать объём, значимый для текущего инструмента.

Минимальные признаки:

1. Объём уровня/принтов значимо больше локального фона:
   - относительно соседних уровней стакана;
   - относительно текущих принтов ленты;
   - относительно объёма последней 5m-свечи/cluster bucket;
   - не ниже операторского `LargeAmountUsd`, если доступен.
2. Действие имеет устойчивость во времени:
   - плотность стоит заранее до подхода цены;
   - или появляется/переставляется повторяемо, не исчезая как fake-density.
3. Действие меняет поведение толпы:
   - перед плотностью закрываются позиции;
   - уровень начинает защищаться обратными принтами;
   - движение формируется за счёт фронтрана участника;
   - после завершения реализации объёма возникает откат.

В коде это должно представляться как confidence/evidence, а не как факт
«участник X». Поля вроде `participant_id` запрещены без источника данных.

### 2.2. Existing signal mapping

| Ручной признак | Existing / planned feature | Статус | Offline проверка | Live-only / ambiguous |
|----------------|----------------------------|--------|------------------|-----------------------|
| Заранее стоящая крупная плотность | `DensityDetected` | Core detector spec | Да: replay orderbook with timestamps | Нельзя доказать намерение участника |
| Плотность быстро снята | `DensityRemoved(fake=true)` | Core detector spec | Да | Может быть нормальный cancel, не обязательно spoofing |
| Плотность разъедают до 1/2–1/3 | `DensityEating` | Core detector spec | Да при синхронизации trade/book | Raw-vs-aggregated tape влияет на timing |
| Завал плотностей | `DensityStack` | Specified sub-detector | Да | «Один участник» не доказуем без order-id |
| Айсберг/подставление | `IcebergSuspected` | Core detector spec | Да частично | Нужна точная event ordering; MetaScalp timestamps live gate |
| Крупные обратные принты на подходе | `TapeBurst` opposite side + `LevelApproach` | Core detector composition | Да | Aggregated trades могут скрыть структуру |
| Затухание ленты перед отскоком | `TapeFade` | Core detector spec | Да | Требует корректной частоты/aggregation calibration |
| Одинаковые объёмы вверх/вниз в диапазоне | `Distribution` + future `SymmetricPrints` | Research/planned | Частично | Нужно отличать раздачу от market-making noise |
| Робот: одинаковые объёмы/цены/интервалы | future `RobotPattern` context | Ambiguous | Только replay-гипотезы | Не торговать live без labeled fixtures |

---

## 3. Формальная модель «неэффективности»

### 3.1. Определение

**Эффективный рынок** — состояние, где коррелирующие инструменты движутся
согласованно, стакан не имеет дыр, маркетмейкеры/роботы поддерживают нормальную
ликвидность, а участник может реализовать объём без резкого воздействия на цену.

**Неэффективность** — отклонение, при котором:

1. крупный участник не может реализовать желаемый объём по желаемой цене и в
   желаемое время без заметного движения цены; или
2. нарушается нормальная связь инструментов/рынков/стакана; и
3. появляется заранее проверяемый сценарий возврата, догоняющего движения или
   короткого stop-anchor.

Ручной источник описывает эти сделки как высоковероятные и допускающие
повышенный объём, но production-бот не должен повышать риск автоматически.
Повышенный объём возможен только после отдельного risk gate и статистической
валидации на labeled replay/live paper выборке.

### 3.2. Иерархия неэффективностей

| Паттерн | Суть | Формальный сигнал/стратегия | Статус |
|---------|------|------------------------------|--------|
| Переставление плотности | Крупная заявка 3–5 раз движется ближе к спреду | `LargeParticipantMove` → planned `LargeParticipantFollow` | Planned, offline-first |
| Реализация объёма | Крупная заявка снимается/ставится выше/ниже спреда, часть исполняется рынком | `LargeParticipantMove` + `TapeBurst`/`DensityEating` | Planned |
| Разжатая пружина | После завершения реализации объёма фронтранеры закрываются, возникает откат | planned `SpringReleaseReversal` | Planned/Phase 5 |
| Повторяющиеся прострелы | Повторный вынос в том же диапазоне с быстрым возвратом | `TapeFlush` + history of returned flushes → `FlushReversal` | Paper; live-gated |
| Импульс/ликвидации | 10–15% за 1–3 пятиминутки, повторные ликвидационные прострелы | `TapeFlush` + external liquidation/OI feed | Live-only until feed |
| Раскорреляция spot/futures/leader | Один рынок ушёл, другой удержан плотностью/роботом | `SpotFuturesDislocation`, `LeaderMove` | Planned/live-gated for spot/futures |
| Раздача | Одинаковые market объёмы вверх/вниз в диапазоне, цена почти не меняется | `Distribution` + future `SymmetricPrints` | Research |
| Искусственная планка | Динамическая граница, отстоящая на 15–20% от spread | No safe detector | Ambiguous/live-only |
| Робот | Одинаковые объёмы, цены, интервалы, высокая скорость | future `RobotPattern` | Ambiguous/research |

---

## 4. Pattern cards

### 4.1. Переставление плотности (`LargeParticipantMove`)

**Source:** `docs/неэфективности.docx`: крупная лимитная заявка переставляется
несколько раз близко к спреду, обычно 3–5 раз. `docs/крупные участники.docx`:
участник переставляет плотность ближе к спреду с намерением реализовать её,
не моментально.

**Input observations:**

- book updates per exact `(ConnectionId, MarketType, Ticker)`;
- best bid/ask/mid;
- rolling level-size baseline and `LargeAmountUsd` if available;
- optional tape confirmation near each placement.

**Entry condition (signal, not trade):**

- level size `>= density_min_size_usd` and stable within `rel_size_epsilon`;
- same side level appears at prices monotonically closer to spread;
- `move_count` in `[3, 5]` within `max_sequence_sec`;
- each placement age `>= min_placement_ms` and not `fake_threshold_ms`;
- gap between removal and reappearance `<= reappear_timeout_sec` (manual: 30s);
- sequence direction is compatible with pressure: ask moving down implies sell
  pressure/resistance; bid moving up implies buy pressure/support.

**Confirmations:**

- `TapeBurst` in direction of expected implementation;
- price follows the moving density without immediate full reversal;
- no stale book/tape state;
- optional `notification_update.BigOrderBookAmount` boosts priority only, not
  a standalone trigger.

**Cancellation:**

- no reappearance within 30s after removal;
- size changes beyond tolerance and no longer qualifies as large;
- level jumps away from spread;
- spread widens beyond global strategy filter;
- book/tape stream stale.

**Stop anchor:**

- for follow trade: behind the current/last large level plus stop buffer;
- if the sequence is used as context only, it may tighten exits of other plans.

**TP/RR:**

- do not assume high potential; manual says higher probability/volume, not
  necessarily higher move. Minimum RR remains governed by `RiskManager`.

**Implementation status:** planned detector already sketched in
`SIGNAL_DETECTION.md § 11.1`; strategy mapped in `STRATEGIES.md § 11.3`.
Needs dedicated task before live use.

**Offline status:** replayable if orderbook deltas preserve enough depth and
clock quality. Identity of «same order» remains probabilistic.

### 4.2. Реализация объёма

**Source:** `docs/неэфективности.docx`: крупная заявка снимается и выставляется
выше/ниже текущей цены спреда, частично реализуясь по рынку; бывает примерно
3 переставления.

**Input observations:**

- `LargeParticipantMove` candidate with removals/replacements;
- aggressive trades near or after removal;
- consumed ratio by `DensityEating` where possible.

**Entry condition (signal):**

- repeated remove/reappear pattern with `move_count ≈ 3`;
- market prints in the expected direction occur between placements;
- price impact is visible and not explained by broad market/leader alone.

**Confirmations:**

- directional `TapeBurst` after each removal;
- density is not simply cancelled and abandoned;
- no leader move against the plan.

**Cancellation:** same as `LargeParticipantMove`, plus missing tape evidence.

**Stop anchor:** current/last density level; if using futures from spot density,
stop must be widened and volume reduced as in § 4.6.

**TP/RR:** quick tactical exit or transition to spring-release reversal when
volume is completed.

**Offline status:** partially replayable. Requires correct trade/book ordering;
MetaScalp aggregation can hide individual executions.

### 4.3. Разжатая пружина (`SpringReleaseReversal`)

**Source:** `docs/неэфективности.docx`: движение создано одним крупным
участником; рынок фронтранил его; после разъедания плотности покупки/продажи
заканчиваются и фронтранеры кроются, давая откат 20–30% движения, иногда 50%.

**Input observations:**

- completed `LargeParticipantMove` or `DensityEating` through the terminal level;
- preceding directional move measured from sequence start;
- `TapeFade` or opposite `TapeBurst` after completion;
- absence of strong leader continuation.

**Entry condition (planned):**

- terminal density consumed/removed after confirmed large-participant sequence;
- price fails to continue within `continuation_timeout_ms`;
- tape intensity in original direction fades;
- expected reversal target at least 20–30% of measured participant-driven move.

**Cancellation:**

- continued aggressive tape in original direction;
- new support density appears behind move;
- leader/market regime confirms breakout, not exhaustion.

**Stop anchor:** beyond post-consumption extremum or reappeared density.

**TP/RR:** TP1 at 20–30% retrace; optional max 50% only in paper/labeled mode.

**Status:** planned/Phase 5. Must not be folded into `BreakoutEatThrough`,
because `DensityEating` there means continuation, while spring-release means
reversal after participant completion.

### 4.4. Повторяющиеся прострелы и ликвидации

**Source:** `docs/неэфективности.docx`: single flush is not enough; after >1%
flush place limits from 50% of flush range to max/min; after 10–15% move in
1–3 five-minute candles, hold/reprice limits in ~10% distance range and close
immediately if filled.

**Existing mapping:** `TapeFlush` and `FlushReversal` cover the core idea of
large outlier print + return. `STRATEGIES.md § 4.5` correctly live-gates
liquidation/OI requirements.

**Entry condition:** only after repeated returned flushes in same range, not on
first event.

**Confirmations:**

- prior flush returned within `back_window_sec`;
- current limit zone lies within previous flush range;
- spread/liquidity allow immediate exit;
- no stale data.

**Cancellation:**

- no return after first flush;
- spread/volatility too wide;
- live liquidation/OI feed unavailable when strategy is configured to depend on it.

**Stop anchor:** local extremum beyond flush band; exits are fast market exits.

**Offline status:** replayable for tape/book-only variant. Liquidation/OI variant
is live-only/external-feed gated.

### 4.5. Раскорреляция spot/futures/leader

**Source:** both DOCX files: spot and futures normally correlate; if spot moves
but futures is held by density, futures should catch up after density is eaten;
leader move may imply delayed alt move when local density/robot releases.

**Existing mapping:** `LeaderMove`/`LeaderLag` covers leader-following for
configured leader streams. `SpotFuturesDislocation` is planned in
`SIGNAL_DETECTION.md § 11.2` and `STRATEGIES.md § 11.2`.

**Required identity invariant:** every comparison must use explicit
`InstrumentId = {ConnectionId, ExchangeId/account context, MarketType, exact
Ticker}` and an explicit mapping between related instruments. String aliases are
insufficient.

**Entry condition:**

- spot/futures or leader/child mapping configured;
- both streams fresh;
- rolling correlation/basis model says current divergence is abnormal;
- blocking density/robot context explains why lagging instrument has not moved;
- density starts eating/removing in expected catch-up direction.

**Cancellation:**

- mapping missing;
- either feed stale;
- divergence within normal basis band;
- density not present or already removed before plan;
- leader reverses before trigger.

**Stop anchor:** behind blocking density. For futures entry from spot density,
manual requires smaller size and wider stop (0.3–0.5% rather than 0.1–0.2%).

**Offline status:** possible only with synchronized multi-instrument replay and
explicit mapping. Without mapping this is live-blocked.

### 4.6. Вход на фьюче от спот-плотности

**Source:** `docs/крупные участники.docx`: if spot and futures prices match,
enter futures near spot density ±5–10 points; during dislocation calculate
spot density distance from spot spread and project to futures spread, or record
futures price when spot reaches density. Futures is more elastic; when spot
density is half eaten futures may already be beyond the density price.

**Formalization:**

- method A: `futures_entry = futures_spread ± distance(spot_spread, spot_density)`;
- method B: snapshot futures mid when spot reaches density;
- require explicit spot↔futures mapping and fresh streams;
- use reduced size and wider stop band.

**Status:** live-only until explicit mapping, multi-feed replay, and contract
fixtures exist. Do not approximate by ticker names.

### 4.7. Раздача / symmetric prints

**Source:** `docs/неэфективности.docx`: equal market volume is thrown up and
down in a spread range so price does not move much, often to transfer/reopen
volume.

**Existing mapping:** `TapeAnalyzer Distribution` detects high volume in narrow
range with persistent spread. This is necessary but not sufficient.

**Needed future feature:** `SymmetricPrints` evidence:

- alternating buy/sell market prints;
- similar sizes within `rel_size_epsilon`;
- narrow price band;
- low net price displacement;
- repeated cadence/interval evidence.

**Status:** research/ambiguous. `Distribution` can be context for avoiding bad
breakout entries, but must not become a standalone live entry until labeled.

### 4.8. Роботы and artificial planks

**Source:** `docs/неэфективности.docx`: robots repeat same volume/price/interval
faster than humans; artificial planks can sit a dynamic distance from spread.

**Formal status:** ambiguous. The available MetaScalp API does not provide actor
identity. Pattern can only be inferred statistically from book/tape cadence.
Use as research labels or manual dashboard context, not live trading trigger.

---

## 5. Traceability matrix

| Manual rule / phrase | Formal condition | Existing artifact | Gap / action | Gate |
|----------------------|------------------|-------------------|--------------|------|
| «Крупная плотность стоит заранее» | `age_ms >= sticky_duration_ms`; size thresholds | `DensityDetector` | Ensure `sticky_min_trade_ms` configured before strategies use it | Offline replay |
| «Плотность крупна относительно стакана/принтов/5m свечи» | DEMA avg level, print size distribution, candle/cluster volume | `DensityDetector`, `TapeAnalyzer`, `cluster-snapshot` | Add 5m-volume comparison to density confidence if not implemented | Offline |
| «На споте плотность чаще настоящая» | spot density gets higher confidence, not automatic trade | none / strategy context | Needs explicit market type in instrument identity | Live/data mapping |
| «Завал плотностей» | same-side density cluster within stop width | `DensityStack` spec | Implement dedicated payload: first/last/width/total | Offline |
| «Разъедание при остатке 1/2–1/3» | `eaten_ratio >= 0.5` and remaining <= 1/3–1/2 | `DensityEating` | Add remaining-ratio payload if absent | Offline with book/trade sync |
| «Переставление 3–5 раз ближе к спреду» | monotonic placement sequence, stable size | `LargeParticipantMove` planned | Create implementation task | Offline-first |
| «Если 30 сек не выставил новую плотность — выход» | reappear timeout | `LargeParticipantMove` planned | Must be strategy exit rule | Offline/live |
| «Реализация объёма частично по рынку» | remove/reappear + directional prints | planned | Needs tape/book join and aggregation calibration | Offline/live gate |
| «Разжатая пружина после дореализации объёма» | terminal consumption + tape fade/opposite burst | planned `SpringReleaseReversal` | Separate strategy task; do not overload breakout | Paper first |
| «Повторный прострел, первый не торгуем» | at least 2 flushes in same band with return | `TapeFlush`, `FlushReversal` | Ensure history requirement in strategy | Paper/live gated |
| «Ликвидационные прострелы после 10–15% за 1–3 5m» | external liquidation/OI + impulse context | ExternalFeeds planned | Live-only until feed | Live-only |
| «Спот вырос, фьюч держит плотность» | basis/correlation dislocation + blocking density | `SpotFuturesDislocation` planned | Explicit spot/futures map required | Live/replay mapping |
| «Фьюч резиновее спота» | reduced size, wider stop 0.3–0.5% | `RiskManager` sizing must support | Add risk rule/config before live | Live gate |
| «Раздача одинаковыми объёмами» | distribution + symmetric print sizes | `Distribution` partial | Research detector needed | Ambiguous |
| «Робот делает одинаково и быстро» | cadence/size/price regularity | none | Research only | Ambiguous |

---

## 6. Production invariants

1. **Evidence, not identity.** Signals may say `large_participant_evidence`, not
   identify a participant.
2. **Instrument identity is explicit.** Any cross-market logic requires exact
   `(ConnectionId, MarketType, Ticker)` and configured mapping.
3. **No live standalone planned signals.** `LargeParticipantMove`,
   `SpringReleaseReversal`, `SpotFuturesDislocation`, `SymmetricPrints`, and
   `RobotPattern` are not live triggers until tests, replay fixtures and gates
   exist.
4. **Tape aggregation is a parameter.** If `AddingTicksForAPeriod != 0`, tape
   detectors must use aggregated calibration or refuse raw-tape assumptions.
5. **Risk stays conservative.** Manual «повышенный объём» is not a default bot
   behavior. It requires separate config, risk acceptance and statistical proof.
6. **Ambiguous patterns can be dashboard context.** They may annotate charts,
   journal entries or manual review, but not submit orders.
7. **Every live-only dependency degrades safely.** Missing liquidation feed,
   stale spot/futures stream or absent mapping disables the corresponding plan.

---

## 7. Implementation follow-ups

These are separate implementation tasks, not part of this formalization:

1. Implement `LargeParticipantMove` detector with replay fixtures for 3–5
   monotonic density moves and 30s reappear timeout.
2. Extend `DensityStack` payload to include `first_price`, `last_price`,
   `width_bps`, `total_size_usd`, and `stop_anchor_price`.
3. Add `remaining_ratio` to `DensityEating` payload for 1/2–1/3 manual trigger
   semantics.
4. Create `SpringReleaseReversal` paper strategy after `LargeParticipantMove`
   has positive examples.
5. Add explicit spot/futures mapping config and multi-instrument replay fixtures
   before implementing `SpotFuturesDislocation` live logic.
6. Research `SymmetricPrints` detector for раздача, starting as dashboard/journal
   context only.
