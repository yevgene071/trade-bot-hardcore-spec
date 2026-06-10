# SIGNAL_DETECTION

Формальные детекторы торговых паттернов. Каждый детектор имеет:
- **Вход:** какие данные читает
- **Алгоритм:** пошаговое описание
- **Пороги:** числа из конфига, с дефолтами
- **Выход:** `Signal` со структурой payload
- **Известные ложные срабатывания** и как от них отстраиваемся

Все числа — **дефолты**, предназначенные для крипто-фьючерсов с высокой ликвидностью
(BTC/ETH/SOL-USDT на Binance/Bybit). Под каждый тикер настройки нужно калибровать
по историческим данным (см. `ROADMAP.md § Фаза 5`).

Ручные торговые основания для детекторов сведены в
`STRATEGY_SOURCE_DIGEST.md`. Если новый detector не связан с ручным паттерном
или с явно описанным MetaScalp/API-событием, он не входит в production scope.

**MetaScalp caveat:** `trade_update` в SDK v1.0.7 может быть агрегирован
сервером через setting `AddingTicksForAPeriod` (default около 200 ms). Детекторы
ленты, которым нужен raw tape, должны либо требовать `AddingTicksForAPeriod=0`
через orderbook settings/operator setup, либо явно работать с aggregated prints
и иметь отдельную калибровку.

---

## 1. DensityDetector — обнаружение плотности

**Плотность** — крупная лимитная заявка (или стакан заявок на одной цене),
которая "держит" уровень и создаёт препятствие для движения цены.

### DensityDetector: вход
- `orderbook_update` события (real-time)
- текущий `OrderBook`

### DensityDetector: алгоритм

```
on_book_update(update):
    for each price_level in update:
        size = price_level.size
        side = price_level.side
        avg_level_size = rolling_average_of_book_level_sizes(last 30 seconds)

        if size >= min_size_vs_avg * avg_level_size
           AND size * price >= min_size_usd
           AND distance_from_mid_bps(price) >= min_distance_bps:

            track(price_level, first_seen_ts = now)

    # проверка времени "стояния"
    for each tracked level:
        if still present in book AND (now - first_seen_ts) >= sticky_duration_ms:
            emit Signal(DensityDetected, price, size, side)
            mark_as_emitted(level)

        if removed_before(fake_threshold_ms):
            # ложная плотность: появилась и сразу снята → признак манипуляции
            emit Signal(DensityRemoved, fake=true)
```

### DensityDetector: пороги (defaults)

| Параметр | Default | Описание |
|----------|---------|----------|
| `min_size_vs_avg` | **10.0** | размер уровня ≥ 10× среднего по стакану |
| `min_size_usd` | **$50,000** | абсолютный минимум (чтобы не ловить плотности на неликвиде) |
| `min_distance_bps` | **5 bps** | не ближе 5 bps от mid — иначе это best, а не плотность |
| `max_distance_bps` | **150 bps** | дальше 1.5% плотность не считаем |
| `sticky_duration_ms` | **2000** | минимум 2 сек на месте, чтобы подтвердить |
| `fake_threshold_ms` | **300** | снятие за <300 мс = "ложная" |

### DensityDetector: payload сигнала
```json
{
  "side": "Ask",
  "price": 65432.0,
  "size": 12.5,
  "size_usd": 818150,
  "age_ms": 3400,
  "ratio_to_avg": 14.2
}
```

### DensityDetector: ложные срабатывания
- **Айсберг-маскировка.** Крупные айсберги выглядят как плотность, но на каждом съедании подставляется новый кусок. Отдельный `IcebergDetector` различает.
- **Имитация.** Плотности часто ставят "для вида" за секунду до съедания. Поэтому `sticky_duration_ms` ≥ 2 сек — и не входим в сделку "на плотность", если её возраст < `sticky_min_trade_ms = 5000`.

### 1.1. DensityEating — активное проедание плотности

Sub-detector к DensityDetector. Эмитит `DensityEating`, когда зафиксированная
плотность **активно разъедается** по рынку и риск пробоя возрастает.

```
on_trade(t):
    # ищем tracked plотность на цене t.price (± cluster_tolerance_bps = 5)
    level = find_tracked_density(t.price, side = opposite(t.side))
    if level is None: return

    register_hit(level, volume = t.size * t.price, ts = now)

    eaten_window = sum(level.hits, last eating_window_ms)
    eaten_ratio  = eaten_window / level.initial_size_usd

    if eaten_ratio >= eating_ratio_threshold
       AND prints_count(level, last eating_window_ms) >= eating_min_prints:
        emit Signal(DensityEating, level.price, eaten_ratio)
```

| Параметр | Default | Описание |
|----------|---------|----------|
| `eating_window_ms` | **3000** | окно наблюдения |
| `eating_ratio_threshold` | **0.5** | за окно прошло ≥ 50% от изначального размера |
| `eating_min_prints` | **5** | минимум 5 принтов по уровню |
| `eating_cooloff_ms` | **1000** | после эмита — пауза, чтобы не дублировать |

**Payload:**
```json
{
  "side": "Ask",
  "price": 65432.0,
  "initial_size_usd": 800000,
  "eaten_usd": 460000,
  "eaten_ratio": 0.575,
  "remaining_ratio": 0.425,
  "prints_in_window": 9
}
```

`remaining_ratio` = `max(0, remaining_size / original_size)`. Enables explicit
1/2–1/3 manual trigger semantics: strategies can check
`remaining_ratio <= 0.5` (half-eaten) or `remaining_ratio <= 0.33` (third-eaten)
without recomputing from `eaten_ratio`. Guarded against zero/negative baselines
(no NaN/Inf).

Если плотность полностью съедена (размер уровня в книге < 20% изначального) —
DensityEating больше не эмитится, вместо этого DensityRemoved (с `fake=false`).

### 1.2. DensityStack — завал плотностей

Ручная стратегия отдельно выделяет "завал плотностей": несколько крупных
заявок в узком диапазоне, где вход делается от первой плотности, а стоп
ставится за последней.

```
on_density_detected(d):
    cluster = find emitted densities on same side within stack_max_width_bps
    if cluster.count >= stack_min_levels
       AND width(cluster) <= stack_max_width_bps
       AND sum(cluster.size_usd) >= stack_min_size_usd:
        emit DensityStack(first_price, last_price, width_bps, total_size_usd, stop_anchor_price, side)
```

| Параметр | Default | Описание |
|----------|---------|----------|
| `stack_min_levels` | **2** | минимум плотностей в завале |
| `stack_max_width_bps` | **20** | диапазон завала не шире нормального стопа |
| `stack_min_size_usd` | **$100,000** | суммарный размер |

**Payload:**
```json
{
  "side": "Ask",
  "first_price": 65400.0,
  "last_price": 65500.0,
  "width_bps": 15.3,
  "total_size_usd": 818000.0,
  "stop_anchor_price": 65500.0,
  "size": 12.5,
  "size_usd": 818000.0
}
```

**Side-aware semantics (defined in code comments and tests):**
- **Ask (resistance) stack:** `first_price` = nearest/lower price (entry zone),
  `stop_anchor_price` = farthest/higher price (stop goes above).
- **Bid (support) stack:** `first_price` = nearest/higher price (entry zone),
  `stop_anchor_price` = farthest/lower price (stop goes below).

`BounceFromDensity` использует `first_price` как зону входа и `stop_anchor_price`
как stop-anchor. `BreakoutEatThrough` использует завал как серию преград:
после разъедания первой плотности ожидается переход к следующей.

Duplicate suppression: the same side + same first/last ticks + same total USD
will emit at most once. Re-arms naturally when levels are removed or refilled.

### DensityDetector: implementation notes (algorithm-grade)

- **`avg_level_size`** — DEMA (двойной EMA, ARCH § 11.6) с `α = 2/(N+1)`,
  `N = 30s · feature_rate_hz`. Не SMA: при появлении новой плотности SMA
  её включает в среднее и порог искажается; DEMA реагирует ~2× быстрее на
  сдвиг распределения.
- **`first_seen_ts` хранение** — `absl::flat_hash_map<PriceTick, LevelMeta>`
  per ticker (см. ARCH § 10.3). PriceTick — fixed-point (§ 11.3), точное
  равенство, нет epsilon-сравнений.
- **Branchless update** path: проверка трёх порогов через bitwise-AND флагов
  (`is_size_ok & is_distance_ok & is_usd_ok`) — нет CPU mispredict на
  стандартном update-уровне (`[[likely]]` в `apply_update`).
- **Hot-path budget:** ≤ 1 µs на book_update (бенчмарк T2-DENSITY).

---

## 2. IcebergDetector — обнаружение айсбергов

**Айсберг** — заявка, у которой видимая часть мала, а реальный объём подставляется частями по мере съедания.

### IcebergDetector: вход
- `trade_update` (сделки)
- `orderbook_update` (книга)

### IcebergDetector: алгоритм

```
on_trade(t):
    # если принт ушёл на best_ask/best_bid, смотрим что случилось с уровнем
    level_before = book_state_before_trade(t.price, t.opposite_side)
    level_after = book_state_after_trade(t.price, t.opposite_side)

    consumed = max(0, level_before.size - level_after.size - t.size)

    # если уровень "просел" меньше, чем было выбито — значит кто-то подставил
    refill = level_before.size - level_after.size
    if t.size > refill AND level_after.size >= level_before.size * 0.5:
        # на уровне осталось столько же (или не сильно меньше), хотя его выбивали
        accumulate_iceberg_evidence(t.price, t.side)

    # после N подтверждений за окно X сек — считаем айсбергом
    if evidence_count(price, window=30s) >= 3
       AND total_consumed_through_level >= iceberg_min_size_usd:
        emit Signal(IcebergSuspected, price, estimated_size)
```

### IcebergDetector: пороги

| Параметр | Default | Описание |
|----------|---------|----------|
| `evidence_count_min` | **3** | сколько раз "подставить" объём, чтобы признать айсбергом |
| `evidence_window_sec` | **30** | окно накопления свидетельств |
| `iceberg_min_size_usd` | **$100,000** | суммарный съеденный через уровень объём |
| `size_retention_ratio` | **0.5** | уровень сохраняет ≥ 50% видимого размера после съедания |

### IcebergDetector: payload
```json
{
  "price": 65432.0,
  "side": "Ask",
  "total_eaten_usd": 245000,
  "refill_events": 5,
  "estimated_remaining_usd": "unknown"
}
```

### IcebergDetector: ложные срабатывания
- На низколиквидных парах — много случайных "переустановок" заявок, особенно ботами-маркет-мейкерами. Поэтому `min_size_usd` высокий.
    - При очень быстром движении — `trade_update` и `orderbook_update` могут приходить не в том порядке. Смягчаем строгий порядок: анализируем оба события в окне ±100 мс.
      *   **Внимание (Джиттер):** `METASCALP_API_CONTRACT.md` не предоставляет явных гарантий по точности временных меток. Необходимо провести контрактные тесты для верификации источника времени. В условиях джиттера окно в 100 мс может потребовать калибровки.


### IcebergDetector: implementation notes (algorithm-grade)

- **Bayesian evidence accumulation** вместо binary counter:
  для каждого подозрительного уровня поддерживаем `posterior P(iceberg | evidence)`
  с prior `P(iceberg) = 0.05` (типичный baseline). Каждое refill-событие
  обновляет:
  ```
  P(refill | iceberg) = 0.85   # likelihood
  P(refill | ¬iceberg) = 0.10
  posterior_new = posterior · L1 / (posterior · L1 + (1−posterior) · L0)
  ```
  Эмитим `IcebergSuspected` при `posterior >= iceberg_posterior_threshold`
  (default **0.80**). Это плавная альтернатива hard-counter `evidence_count_min=3`,
  работает быстрее на стабильных айсбергах и устойчивее к шуму.
- **fallback** — при `iceberg_posterior_threshold` отсутствует в конфиге,
  работает legacy counter (порог 3) для обратной совместимости тестов.
  - **Trade ↔ orderbook_update ordering:** окно сшивания событий — `event_join_window_ms = 100`,
    ring-buffer last-100-events на тикер (lock-free), при поступлении
    любого из двух событий ищется парный в окне через `std::lower_bound`
    по timestamp.
    *   **Мониторинг:** Надежность механизма зависит от точности временных меток MetaScalp API. В `Phase 0` (T0-MONITOR-TIMESTAMPS) добавлены метрики для мониторинга задержки и джиттера.

---

## 3. TapeAnalyzer — классификация ленты

Классификация состояний ленты сделок на 4 типа:
- **Burst** — резкий всплеск агрессии в одну сторону
- **Fade** — затухание активности
- **Flush / Прострел** — единичная очень крупная сделка, цена уходит и возвращается
- **Раздача (Distribution)** — активная торговля в узком диапазоне с сохранением спреда

### TapeAnalyzer: вход
- `TradeStream` (скользящие окна 1/5/30 сек)
- текущий best bid/ask

### Sub-detector 3.1. TapeBurst

```
compute for window W (e.g. 5 sec):
    buy_vol = sum of trade sizes * price where side == Buy
    sell_vol = sum of trade sizes * price where side == Sell
    total_vol = buy_vol + sell_vol
    dominant_ratio = max(buy_vol, sell_vol) / max(1, min(buy_vol, sell_vol))

    if dominant_ratio >= burst_ratio
       AND total_vol >= burst_min_volume_usd
       AND prints_per_sec >= burst_min_rate:
        emit TapeBurst(side=Buy/Sell, ratio, vol)
```

Пороги: `burst_ratio=3.0`, `burst_min_volume_usd=50000`, `burst_min_rate=5 prints/sec`.

### Sub-detector 3.2. TapeFade

```
peak_rate = max(prints_per_sec over last 60 sec)
current_rate = prints_per_sec in last 2 sec

if current_rate <= peak_rate * fade_ratio  (fade_ratio = 0.2)
   AND peak_rate >= fade_min_peak_rate  (=10 prints/sec)
   AND fade_age_sec <= fade_max_delay (=30 sec):
    emit TapeFade(prev_peak, current, direction_of_prev_burst)
```

**Зачем:** затухание после импульса часто предшествует развороту (отскок после пробоя ради выноса стопов).

### Sub-detector 3.3. TapeFlush / Прострел

```
on_trade(t):
    if t.size_usd >= flush_min_size_usd   (=$200,000)
       AND t.size_usd >= N * avg_print_size_30s (N = 20):
        before_price = mid_before_trade
        after_price = mid_after_trade

        delta_bps = |after - before| / before * 10000

        if delta_bps >= flush_min_move_bps (=15):
            emit TapeFlush(size, delta_bps, direction)
            # запускаем наблюдение: вернётся ли цена за окно back_window_sec=10?
```

Если цена возвращается за 10 сек — классический "прострел" (маркетмейкеры вернули цену).

### Sub-detector 3.4. Distribution / Раздача

```
window = last 60 sec
price_range = max_price - min_price
spread_avg = avg(best_ask - best_bid) over window
volume = sum_size_usd over window

if price_range_bps <= distribution_max_range_bps (=20)
   AND volume >= distribution_min_volume_usd (=$500,000)
   AND spread_avg_bps >= distribution_min_spread_bps (=2):
    # много сделок, узкий диапазон, сохраняющийся спред
    emit Distribution(range, volume)
```

### TapeAnalyzer: implementation notes (algorithm-grade)

- **TapeBurst через Hawkes process intensity** (см. ARCH § 2.3 TradeStream):
    *   **Критическое замечание:** Точность оценки `prints_per_sec` и интенсивности процесса Хоукса критически зависит от точности временных меток в `trade_update`. Если MetaScalp агрегирует события или предоставляет неточное время, это может существенно исказить результаты и привести к ложным сигналам или задержкам в обнаружении всплесков активности.
  не «считаем объёмы в окне 5 сек», а ведём `λ(t) = μ + Σ α·exp(-β(t-tᵢ))` —
  exponentially-decaying intensity. `μ` — фоновая интенсивность из 30s
  окна (Welford-EMA). Burst = `λ_buy(t) / λ_sell(t) >= burst_ratio` И
  `λ_total(t) >= k·μ` (default k=3). Преимущество: реакция мгновенная
  (на каждом trade, а не каждые 5 сек), ложные срабатывания на единичных
  принтах подавляются благодаря экспоненциальному затуханию.
  - параметры: `α=1.0`, `β = ln(2) / half_life_sec`, `half_life_sec=2.0`
    (decay 50% за 2 сек) — конфиг `[signals.tape.hawkes]`
- **TapeFade через CUSUM (Page 1954) change-point detector:**
  ```
  S_n = max(0, S_{n-1} + (target - x_n) - drift)
  alarm if S_n >= threshold
  ```
  где `x_n = prints_per_sec`, `target = peak_rate * fade_ratio` (default 0.2),
  `drift = 0.05·peak_rate`, `threshold = fade_cusum_h` (default **5**).
  CUSUM детектирует устойчивое снижение **на 200–500 мс быстрее** наивного
  «current ≤ peak * 0.2 в течение N сек» — критично для timing'а отскока.
- **TapeFlush:** поскольку каждый event анализируется через Hawkes, flush
  идентифицируется как **outlier** в распределении сделок: `t.size_usd > q99(trade_size_distribution)` где q99 рассчитывается через **t-digest** (см. ARCH § 11) на 30-минутном окне. Альтернатива хардкоженному `flush_min_size_usd` — адаптивно к ликвидности тикера.
- **Distribution:** оценивается через **stdev цены / mid_price** на 60s
  окне (Welford). Узкий диапазон ⇔ low stdev. Реализация дешёвая, не
  требует max-min пересчёта на каждом trade.
- **Hot-path budget:** ≤ 2 µs на trade.

---

## 4. LevelDetector — горизонтальные уровни

### LevelDetector: вход
- история сделок и best bid/ask за `lookback_min` минут
- текущая цена
- **cluster-snapshot** (volume profile) с MetaScalp `GET /api/connections/{ConnectionId}/cluster-snapshot?Ticker&TimeFrame&ZoomIndex` (см. `ARCHITECTURE.md § 2.3`) — кластеры объёма как **второй источник уровней**: точки концентрации объёма на M15/H1 — сильные горизонтальные уровни

### LevelDetector: алгоритм

```
# шаг 1: найти экстремумы
extremes = []
for each price p over lookback window:
    if p is local_max over ±window_sec (e.g. ±30 sec) AND magnitude >= min_reversal_bps (=15):
        extremes.append(p, ts, type=high)
    if p is local_min over ±window_sec:
        extremes.append(p, ts, type=low)

# шаг 2: кластеризация по цене
clusters = []
for e in extremes:
    nearest = find cluster where |cluster.price - e.price|_bps <= cluster_tolerance_bps (=10)
    if nearest: nearest.add(e)
    else: clusters.append(new cluster(e))

# шаг 3: валидные уровни
for c in clusters:
    if c.size >= touches_min (=2)
       AND c.time_span_sec >= level_min_age_sec (=300):
        level = {
            price: c.centroid_price,
            strength: c.size,      # чем больше касаний, тем сильнее
            last_touch_ts: c.last_ts,
            type: c.dominant_type  # resistance (from highs) / support (from lows)
        }
        emit LevelFormed(level)
```

### LevelDetector: пороги

| Параметр | Default |
|----------|---------|
| `lookback_min` | **60** мин |
| `min_reversal_bps` | **15** bps (0.15%) |
| `cluster_tolerance_bps` | **10** bps |
| `touches_min` | **2** |
| `level_min_age_sec` | **300** (5 мин) |
| `cluster_snapshot_timeframes` | `["M15","H1"]` |
| `cluster_min_volume_pct` | **5%** (объём в bucket'е ≥ 5% от суммы H1 → кандидат на уровень) |

### LevelApproach

```
on_frame:
    for each known level:
        dist_bps = (price - level.price) / price * 10000

        if |dist_bps| <= approach_trigger_bps (=10)
           AND we are moving towards level (sign of price_change agrees with direction to level):
            emit LevelApproach(level, dist_bps)
```

### LevelRejection / LevelBreak

```
on_trade(t):
    for each level being approached:
        if t.price touches level.price ± cluster_tolerance_bps:
            # ждём N сек исхода
            after_N_sec (=10):
                if mid moved away from level by >= rejection_distance_bps (=20)
                   AND didn't close beyond level:
                    emit LevelRejection(level)
                elif mid moved through by >= break_distance_bps (=25)
                     AND volume through level >= break_min_volume_usd (=$30k):
                    emit LevelBreak(level)
```

### LevelDetector: implementation notes (algorithm-grade)

- **DBSCAN clustering** экстремумов вместо наивного «найти ближайший
  cluster и добавить»:
  - `eps = cluster_tolerance_bps` (default 10 bps в price-space)
  - `min_pts = 2` (как раз `touches_min`)
  - **Преимущество:** не требует known-N кластеров, корректно обрабатывает
    «висящие» одиночные экстремумы (noise), стабильнее на разреженных
    данных. Реализация — `mlpack::dbscan` или собственная (50 строк).
- **Cluster-snapshot integration через KDE** (Kernel Density Estimation):
  ```
  density(price) = Σᵢ (volume_i · K((price - price_i) / bandwidth))
  K = Gaussian kernel
  bandwidth = silverman_rule(prices) · cluster_kde_smoothness
  ```
  Локальные максимумы плотности на M15/H1 footprint'е → кандидаты на
  уровни. Это плавная альтернатива hard-threshold `cluster_min_volume_pct`,
  ловит «зону концентрации» а не точный bucket. SIMD-ускорение через
  xsimd (см. ARCH § 10.5) — KDE на 200 точках × 30 кандидатов ≤ 50 µs.
- **Уровень-confidence** = взвешенное произведение:
  - `touches_score = min(1, c.size / 5)` — 5+ касаний даёт max
  - `kde_score = density_at_centroid / max_density_in_window`
  - `recency_score = exp(-age_min / 60)` — затухание за час
  - итог `confidence = 0.4·touches + 0.4·kde + 0.2·recency` ∈ [0,1] →
    в Signal payload, используется стратегиями для приоритизации.
- **`min_reversal_bps`** через DEMA-сигнал, а не fixed cutoff (адаптивно
  к волатильности тикера: на high-vol тикере 15 bps это шум, на low-vol — настоящее движение).
- **Hot-path budget:** O(N log N) на batch (10 sec), не на каждом trade.
  LevelDetector работает на feature-frame timer (10 Гц), не on_trade.

---

## 5. ApproachAnalyzer — качество подхода к уровню

Ключевая фильтрация: от того, **как** цена подошла к уровню, зависит что делать на уровне.

### Три типа подхода

#### 5.1. Импульсный (impulse) — "экспонента", "без пролива"
```
approach_window = last 30 sec before touching level
distance_travelled_bps = |price_start - level.price| / level.price * 10000

if distance_travelled_bps >= impulse_min_distance_bps (=30)
   AND num_pullbacks(approach_window) <= impulse_max_pullbacks (=1)
   AND avg_speed_bps_per_sec >= impulse_min_speed (=0.8):
    classify = "impulse"
```

**Значение:** после импульсного подхода к уровню вероятность отскока выше (но и риск пробоя существует — "пружина").

#### 5.2. Медленный (slow)
```
if distance_travelled_bps >= slow_min_distance_bps (=30)
   AND num_pullbacks >= slow_min_pullbacks (=3)
   AND approach_duration_sec >= slow_min_duration (=120):
    classify = "slow"
```

**Значение:** медленный подход = накопление позиций → после пробоя уровня движение сильнее (классический breakout setup).

#### 5.3. С проторговкой (consolidation)
```
if last Nsec (=60) price stayed within consolidation_range_bps (=10) of the level
   AND volume > consolidation_min_volume_usd (=$100k):
    classify = "consolidation"
```

**Значение:** идёт накопление/распределение на уровне. Жди пробой или отскок — вход на подтверждение.

### ApproachAnalyzer: payload

```json
{
  "level_price": 65000.0,
  "type": "impulse",  // impulse / slow / consolidation
  "pullbacks": 1,
  "speed_bps_per_sec": 1.2,
  "approach_duration_sec": 24,
  "distance_bps": 48
}
```

### ApproachAnalyzer: implementation notes (algorithm-grade)

- **3-state HMM** для классификации режима подхода (impulse / slow /
  consolidation) вместо if-else пороговой логики:
  - state space: `S = {Impulse, Slow, Consolidation}`
  - observations: 30-сек вектор `[speed_bps_per_sec, num_pullbacks_per_10s, dist_per_window_bps]`
  - emission: Gaussian mixture (per-state mean + cov, обучаются раз в неделю на исторических labeled events из T2-LABELING)
  - transition matrix: фиксированная (impulse→consolidation→slow редкие переходы)
  - inference: **Forward algorithm** O(N·S²) на 30-сек окне → posterior probabilities → argmax
  - **fallback** при недостатке training data — старые if-else пороги
- **Pullback detection через ZigZag** (классический теханализ-алгоритм)
  с min_swing = `pullback_min_bps` (default 5 bps) — устойчивее к шуму
  чем счёт локальных экстремумов на raw price.
- **Speed через линейную регрессию** OLS по точкам в окне (Welford-based),
  не как `(p_end - p_start) / duration` — устойчивее к шуму на старте/конце.
- **Hot-path budget:** O(window_size) на каждом feature-frame (10 Гц) — ≤ 50 µs.

---

## 6. LeaderSignal — сигнал от поводыря

**Status:** `gated` input for `LeaderLag`. `LeaderMove` is source-backed as a
formal proxy for manual leader/follower раскорреляция, but it is executable only
when configured leader mapping, correlation, and staleness gates pass. It does
not by itself prove spot/futures dislocation or robot/density release; those
variants remain `phase-later` until explicit instrument identity and replay
fixtures exist.

**Поводырь** — коррелированный инструмент, который двигается первым. В крипте:
BTC → альткоины; ETH → DeFi-альты; SPX/NDX → фондовые.

### LeaderSignal: вход
- `LeaderTracker` — цена поводыря
- цена основного инструмента

### LeaderSignal: алгоритм

```
on_frame:
    leader_change = leader.price_change_5s  # %
    our_change = primary.price_change_5s

    # шаг 1: корреляция должна быть высокой
    corr = leader.rolling_correlation_60s
    if |corr| < leader_min_correlation (=0.6):
        return   # без корреляции сигнал не надёжен

    # шаг 2: поводырь движется, а мы — нет
    if |leader_change| >= leader_move_min_pct (=0.3)   # >= 0.3%
       AND sign(leader_change) == sign(corr):          # в направлении, которое диктует корреляция
        expected_change = leader_change * corr
        lag_diff = expected_change - our_change

        if |lag_diff| >= leader_lag_min_pct (=0.15):   # отставание >= 0.15%
            # у нас есть опцион подтянуться
            direction = sign(lag_diff)
            emit LeaderMove(direction, expected_change, actual=our_change, lag_bps)
```

### LeaderSignal: пороги

| Параметр | Default | Описание |
|----------|---------|----------|
| `leader_min_correlation` | **0.6** | rolling 60-sec Pearson |
| `leader_move_min_pct` | **0.3%** | минимальное движение поводыря для сигнала |
| `leader_lag_min_pct` | **0.15%** | отставание нашего инструмента |
| `lag_max_age_ms` | **3000** | сигнал устаревает за 3 сек |

### LeaderSignal: payload
```json
{
  "leader_ticker": "BTC_USDT",
  "leader_change_5s_pct": 0.42,
  "our_change_5s_pct": 0.08,
  "correlation_60s": 0.78,
  "expected_change_pct": 0.327,
  "lag_pct": 0.247,
  "direction": 1
}
```

### LeaderSignal: ложные срабатывания
- **Раскорреляция события.** Иногда альт двигается по своим новостям — сильный собственный драйвер может ломать корреляцию. Поэтому `corr` считается rolling и мы откидываем сигнал при `|corr| < 0.6`.
- **Шум на коротких окнах.** 5-секундное окно зашумлено; если `leader_change < 0.3%` — игнорируем.

### LeaderSignal: implementation notes (algorithm-grade)

- **Online Pearson correlation** через extended Welford (см. ARCH § 2.3
  `LeaderTracker`) — не пересчитываем окно, всё O(1) на sample. Численно
  стабильно при near-equal values (классическая float-катастрофа Σx²/n−(Σx/n)² избегается).
- **Лаг через Kalman filter** (см. ARCH § 2.3): state = `[lag_ms, lag_drift]`,
  observation = текущий argmax cross-correlation на 10-сек окне (с шагом 100 мс).
  Kalman гасит шум наблюдений и трекает дрифт лага во времени — `confidence`
  из `P` covariance используется при формировании `LeaderMove.confidence`.
- **Cross-correlation argmax** через **FFT** на warmup (60 sec history):
  `IFFT(FFT(x) · conj(FFT(y)))` за O(N log N) вместо O(N²) naive.
  Реализация — `pocketfft` или `kissfft`, на 600 sample (60 sec × 10 Гц)
  ~100 µs (одна warmup-операция при первом spawn LeaderTracker).
- **Раскорреляция-детектор** через **CUSUM** на residual `corr_t - corr_baseline`:
  раннее предупреждение что корреляция ломается (новостной шок) — на
  300–500 мс быстрее наивного порога `corr < 0.5` (используется как
  early-exit в `LeaderLag § 3.7 — Инвалидация после входа`).
- **Hot-path budget:** ≤ 0.5 µs per feature-frame (Welford-update + Kalman-step).

---

## 7. Сводная таблица порогов

Все пороги — из `config.toml` секций `[signals.*]`.

| Детектор | Ключевой порог | Default |
|----------|---------------|---------|
| Density | `min_size_vs_avg` | 10× |
| Density | `min_size_usd` | $50k |
| Density | `sticky_duration_ms` | 2000 |
| Density | `stack_min_levels` | 2 |
| Density | `stack_max_width_bps` | 20 |
| Density | `stack_min_size_usd` | $100k |
| Iceberg | `evidence_count_min` | 3 |
| Iceberg | `iceberg_min_size_usd` | $100k |
| TapeBurst | `burst_ratio` | 3.0 |
| TapeBurst | `burst_min_volume_usd` | $50k |
| TapeFlush | `flush_min_size_usd` | $200k |
| TapeFlush | `flush_min_move_bps` | 15 |
| Level | `touches_min` | 2 |
| Level | `cluster_tolerance_bps` | 10 |
| Approach | `impulse_min_speed` | 0.8 bps/sec |
| Approach | `slow_min_pullbacks` | 3 |
| Leader | `leader_min_correlation` | 0.6 |
| Leader | `leader_lag_min_pct` | 0.15% |

---

## 8. Проверка детекторов

Для каждого детектора в фазе M2.7 готовится размеченная вручную (или
полу-автоматически) выборка:
- минимум **200 положительных** примеров (подтверждённых) — ранее было 50,
  но 50 событий статистически слабо для precision/recall с CI ≤ ±10%
- минимум **400 отрицательных** (похожих, но не триггер) — соотношение 1:2

**Доверительный интервал.** При precision=0.70 и n_positive=200, 95%-CI
Wilson'а = [0.63, 0.76] — допустимая точность для принятия решения
о переходе к Фазе 3. При n=50 тот же CI = [0.56, 0.81] — слишком широко.

Метрики (единый стандарт для всех детекторов):
- **Precision** ≥ 70% (из всех срабатываний — 70% настоящие)
- **Recall** ≥ 50% (из всех настоящих — поймали 50%)

Precision важнее recall: лучше пропустить сигнал, чем сгенерировать ложный.
Оба порога обязательны — TASK_SPECS T2-* AC ссылаются на эту секцию.

Сбор выборки — отдельный тикет T2-LABELING. Формат: JSONL
`replay/labels/<ticker>_labels.jsonl` со схемой
`{timestamp_ns, signal_kind, is_true_positive, evidence_notes}`.

---

## 9. LiquidationDetector — отдельный детектор (ExternalFeed-based)

**Status:** `phase-later` live gate for `FlushReversal`. Current paper/offline
`FlushReversal` may use repeated `TapeFlush`, but live-grade `FlushReversal`
must require `LiquidationFlush` plus open-interest/history confirmation. Plain
`TapeFlush` or `FlushNoLiq` must not satisfy live mode.

Планируется в Фазе 5 вместе с FlushReversal. Использует внешний фид
Binance `<symbol>@forceOrder` (см. ARCH § 2.12 — external_feeds.liquidations).

### LiquidationDetector: вход
- WS-поток ликвидаций Binance (перпы)
- `TapeFlush` сигнал собственного TapeAnalyzer

### LiquidationDetector: алгоритм

```
on_liquidation(L):
    if L.size_usd >= liq_min_size_usd (=$50k):
        buffer_liquidation(L, window=liq_correlation_window_sec=3)

on_tape_flush(F):
    # корреляция: был ли крупный liquidation event в окне ±3 сек?
    liqs = buffered_liquidations(F.ts - 3s, F.ts + 3s)
    liq_sum = sum(l.size_usd for l in liqs where sign(l.side) == sign(F.direction))

    if liq_sum >= F.size_usd * liq_correlation_ratio (=0.5):
        emit LiquidationFlush(F.price, F.direction, liq_sum)
    else:
        # TapeFlush без ликвидаций = шумный принт, не капитуляция
        emit FlushNoLiq(F.price)
```

Использование в T5/live-grade режиме: FlushReversal стратегия (STRATEGIES § 4)
входит только при `LiquidationFlush`, не при `FlushNoLiq` — это отличает
капитуляционные флэши от шумовых одиночных принтов. Текущий кодовый
FlushReversal — `gated` paper/offline prototype без liquidation hard gate;
детали доводки — в T5-FLUSH.

---

## 10. Композиция сигналов — для стратегий

Сами по себе сигналы не ведут к сделке. Сделку принимает **стратегия**
на основе композиции сигналов. См. `STRATEGIES.md`.

Пример: "отскок от плотности" требует ОДНОВРЕМЕННО:
- `DensityDetected` на уровне (противоположная сторона от нашего направления)
- `LevelApproach` к цене этой плотности
- классификация подхода как `impulse`
- `TapeFade` (ленту не ломают — агрессия затихает)
- отсутствие `LeaderMove` в сторону пробоя

Подробнее — `STRATEGIES.md § BounceFromDensity`.

---

## 11. Planned detectors from manual strategy digest

Эти детекторы нужны для полной формализации ручных стратегий, но не должны
использоваться в live до появления тестов и replay fixtures.

### 11.1. LargeParticipantMove

Покрывает ручные паттерны "переставление плотности" и "реализация объёма".

Сигнал появляется, когда одна крупная заявка:

- сохраняет близкий размер (`rel_size_epsilon`);
- 3-5 раз переставляется ближе к spread;
- живёт достаточно долго на каждой цене;
- не является мгновенной fake-density;
- после снятия либо появляется снова в течение 30 сек, либо паттерн
  инвалидируется.

Выходной signal должен содержать `move_count`, `first_price`, `last_price`,
`size_usd`, `direction`, `age_sec`, `last_seen_ts`.

### 11.2. SpotFuturesDislocation

Покрывает ручную раскорреляцию спот/фьюч. Детектор запрещён без явного mapping
тикеров. Строковые эвристики вида "убрать PERP из имени" запрещены в live.

Вход:

- spot price stream;
- futures price stream;
- rolling basis / correlation;
- stale-state обоих источников.

Сигнал:

```
basis_bps = (futures_mid - spot_mid) / spot_mid * 10000
if abs(basis_bps - rolling_basis_mean) >= dislocation_min_bps
   AND corr_60s >= min_corr
   AND blocking_density_exists:
    emit SpotFuturesDislocation(expected_direction, basis_bps)
```

### 11.3. ContinuationSetup

Покрывает ручную сделку "продолжение движения", когда первичный пробой уже
пропущен. Это не самостоятельный entry без стакана: detector должен требовать
новый stop-anchor в направлении движения.

Условия:

- recent `LevelBreak` или сильный `LeaderMove`;
- новое `DensityDetected`/`DensityStack` в поддержку;
- цена не ушла дальше `max_missed_move_bps`;
- до следующего уровня остаётся минимум `min_rr_ratio`.
