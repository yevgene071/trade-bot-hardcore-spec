# FN-002 — Weak Breakout, Potential, Trade Plan Source Map

This report is the traceability artifact for `docs/ФАКТОРЫ СЛАБОГО ПРОБОЯ.docx`, `docs/ПОТЕНЦИАЛ.docx`, and `docs/ПЛАН СДЕЛКИ.docx`.

## Extraction method and reproducibility

Deterministic extraction command used in this worktree:

```bash
mkdir -p reports/fn-002/docx-text
python3 - <<'PY'
import zipfile, re, pathlib, hashlib
from xml.etree import ElementTree as ET
files=['docs/ФАКТОРЫ СЛАБОГО ПРОБОЯ.docx','docs/ПОТЕНЦИАЛ.docx','docs/ПЛАН СДЕЛКИ.docx']
outdir=pathlib.Path('reports/fn-002/docx-text')
ns={'w':'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
for f in files:
    p=pathlib.Path(f); data=p.read_bytes(); xml=zipfile.ZipFile(p).read('word/document.xml')
    root=ET.fromstring(xml); lines=[]
    for para in root.findall('.//w:p', ns):
        text=''.join(node.text or '' for node in para.iter() if node.tag == '{%s}t'%ns['w'])
        text=re.sub(r'[ \\t]+',' ',text).strip()
        if text: lines.append(text)
    out=outdir/(p.name+'.txt')
    with out.open('w',encoding='utf-8') as w:
        w.write(f'# Source: {f}\n# sha256: {hashlib.sha256(data).hexdigest()}\n')
        w.write(f'# extracted_paragraphs: {len(lines)}\n')
        w.write('# method: python zipfile word/document.xml paragraph text-node extraction; whitespace collapsed\n')
        for i,line in enumerate(lines,1): w.write(f'{i:04d}: {line}\n')
PY
```

Generated stable source text:

| Source | Extracted text | SHA-256 | Paragraphs |
|---|---|---:|---:|
| `docs/ФАКТОРЫ СЛАБОГО ПРОБОЯ.docx` | `reports/fn-002/docx-text/ФАКТОРЫ СЛАБОГО ПРОБОЯ.docx.txt` | `be2e2ac26d1da72df5cf8a9901bb64c905801482e39a9f478a399c02fc7bd50a` | 59 |
| `docs/ПОТЕНЦИАЛ.docx` | `reports/fn-002/docx-text/ПОТЕНЦИАЛ.docx.txt` | `61708f648da6b40c0f9705395bac222720e333420d8c47197b0c909857048bc8` | 33 |
| `docs/ПЛАН СДЕЛКИ.docx` | `reports/fn-002/docx-text/ПЛАН СДЕЛКИ.docx.txt` | `3305a8e05007f538842b7b8b3c6a0069566feac4f99ccce83c61d1baaa1f1e68` | 86 |

Status labels (only these four values are used in the rule table):

- `implemented` — current docs/code/config/tests already cover the rule with offline-verifiable data and enough traceability for production use.
- `missing` — source-backed and offline-verifiable, but current implementation/docs/tests lack a rule, stable evidence/rejection name, or coverage.
- `ambiguous` — manual wording needs human confirmation or a detector definition before production implementation.
- `live-only` — requires unavailable/live feed/API semantics and must not be silently implemented.

The generated `reports/fn-002/docx-text/*.txt` files are intentional review artifacts, not scratch temp files: they preserve stable line numbers and input hashes so later docs/tests can cite exact manual-source paragraphs following the FN-001 traceability pattern.

## Rule source map

| Rule ID | Source quote / stable lines | Normalized rule | Required observations | Status | Current coverage evidence | Intended action |
|---|---|---|---|---|---|---|
| WB-01 | `ФАКТОРЫ...` lines 0001-0003: absence of growing volume means no participant interest. | Breakout requires growing relative volume/interest immediately before the level; missing volume is a hard reject with stable traceability. | 5s vs 30s side volume, tape burst. | implemented | `BreakoutEatThrough` records `WeakBreakoutLowParticipation`; exact unit test covers the reason. | Keep config/docs/tests synchronized. |
| WB-02 | `ФАКТОРЫ...` lines 0004-0010: enter when density is eaten to 1/2-1/3 and no contra factors remain. | Density-eating breakout is only valid when the blocking density is mostly consumed but not fully gone, with no active contra factors and stable rejection names. | `DensityEating` original/remaining size, side, recent contra signals. | implemented | `BreakoutEatThrough` records `WeakBreakoutEatingTooEarly` / `WeakBreakoutDensityFullyEaten`. | Add/keep exact tests for both edge cases. |
| WB-03 | `ФАКТОРЫ...` lines 0011-0016: protected level/density lets price trade before a strong breakout. | A defended level is a quality context: prior density/contra prints make the level meaningful but also require waiting for defender to be overwhelmed. | sticky density/cluster, contra prints, approach/trade context. | ambiguous | Docs mention protected levels in `STRATEGIES.md §0.10`; code has no explicit defender identity model. | Keep as docs/taxonomy; do not add identity heuristics. Use existing density and tape evidence only. |
| WB-04 | `ФАКТОРЫ...` line 0018: supports behind accelerate breakout and let stops hide behind them. | Breakout requires support behind the move within a bounded range; missing support/free-space behind is a hard reject with stable traceability. | Recent `DensityDetected`/`IcebergSuspected` on support side and distance to density. | implemented | `BreakoutEatThrough` records `WeakBreakoutNoSupportBehind`; exact unit test covers the reason. | Keep support proxy documented separately from full liquidity model. |
| WB-05 | `ФАКТОРЫ...` lines 0019-0025: many small contra limit orders consume breakout capital and make the move weak. | Contra order-book resistance cluster ahead of breakout is a hard reject or quality downgrade when too large relative to entry density. | Recent contra `DensityDetected`/cluster volume ahead, original density size. | implemented | `BreakoutEatThrough` records `WeakBreakoutContraResistanceCluster`; exact unit test covers the reason. | Keep config/docs/tests synchronized. |
| WB-06 | `ФАКТОРЫ...` lines 0030-0033: dense support helps stopping out; thin support is dangerous. | Support-side liquidity must be sufficient for stop execution; the current support-density proxy is not a full holey-book/slippage model. | Support-side depth/density/iceberg; deterministic slippage/liquidity model. | missing | Current implementation only proxies via required support density/iceberg; `RiskManager` validates stop distance, not executable liquidity/slippage. | Document proxy explicitly; create/follow future task for deterministic liquidity model if required. |
| WB-07 | `ФАКТОРЫ...` lines 0034-0038: leader should move with breakout; sharp opposite leader move can create bounce. | Contra leader move/correlation is a hard reject before entry and a post-entry invalidation with stable names. | `leader_change_1s`, `leader_correlation`, `LeaderMove.lag_pct`. | implemented | `BreakoutEatThrough` records `WeakBreakoutLeaderContra`; post-entry close reason remains `LeaderContraPostEntry`; pre-entry exact test added. | Keep leader threshold docs/config synchronized. |
| WB-08 | `ФАКТОРЫ...` lines 0039-0048: active tape, rising speed/prints, no fades/reversals. | Tape must be directed and active; same-side TapeFade or opposite print dominance rejects/degrades the breakout with stable traceability. | `TapeBurst`, `tape_aggression`, `TapeFade`, print size/rate. | implemented | `BreakoutEatThrough` records `WeakBreakoutNoTapeBurst`, `WeakBreakoutTapeAggressionLow`, and `WeakBreakoutTapeFaded`; exact fade/aggression tests added. | Keep print-size/rate extensions separate until detector support exists; side-less production `TapeFade` is treated as aggregate fade and rejected conservatively. |
| WB-09 | `ФАКТОРЫ...` lines 0049-0053: new resistance, tape reversal, cluster/graphic resistance, post-impulse freeze require exit. | After entry, new obstacles, tape reversal, or post-impulse tape freeze are invalidations/early exits. | Post-entry signals and obstacle levels. | missing | Code exits on `TapeFade` and contra `LeaderMove`; no explicit post-entry new-obstacle/free-space/freeze manager exists. | Document as post-entry management gap; do not conflate with pre-entry potential rule `POT-05`. Create follow-up if not implemented here. |
| WB-10 | `ФАКТОРЫ...` lines 0055-0058: scale out by tape speed/R:P; if price rolls back strongly and tape not with us, exit; leave 2/10 for trend. | Management: active tape exits near spread, slower move exits in R:P zone, strong rollback without tape exits, small runner may remain. | Executor state, tape speed, price progress, partial fill management. | missing | TradePlan has `tp1_size_ratio`, timeouts, follow-through; executors do not encode this full management schema. | Document schema; avoid executor rewrite unless existing fields require consistency. Create follow-up if runtime partial-management gap remains. |
| POT-01 | `ПОТЕНЦИАЛ` lines 0002-0005, 0008-0010: market phase/volatility/trend; ranges make breakouts weak. | Trend and volatility increase potential; sideways/range context downgrades or blocks breakout. | Trend/range detector, volatility, price structure. | ambiguous | `FeatureFrame` has volatility but no formal trend/range phase detector in breakout strategy. | Document as potential factor; do not implement range classifier without detector. |
| POT-02 | `ПОТЕНЦИАЛ` lines 0006, 0016-0019: vertical volume, slow approach/trading, repeated touches increase breakout potential. | Potential improves with vertical volume, slow pre-level trade, and more touches/stop accumulation, but complete compression/touch scoring is not yet production-defined. | Volume surge, approach speed, `LevelApproach.touches`, level age. | missing | Relative volume gate and `TradePlan.approach_count` storage exist, but no complete pre-level compression/touch potential model or tests exist. | Document implemented sub-signals vs missing model; do not invent compression detector. |
| POT-03 | `ПОТЕНЦИАЛ` lines 0011-0015, 0025: higher-timeframe and visible levels matter. | Targets/levels must be market-visible; higher timeframe/V-shape/global formations increase potential. | Level detector timeframe/visibility metadata. | ambiguous | `FeatureFrame` has nearest support/resistance only; no timeframe/visibility payload. | Mark docs-only until detector payload exists. |
| POT-04 | `ПОТЕНЦИАЛ` lines 0026-0033: trend consolidation, shakeout, broken level/retest, range breakout add potential. | Shakeout/retest/context can improve target confidence. | False-break/retest/range pattern detector. | ambiguous | Some concepts in docs, no explicit production detector payload for all. | Keep as source taxonomy; no production implementation. |
| POT-05 | Cross-source: `ПОТЕНЦИАЛ` lines 0004-0005, `ФАКТОРЫ...` lines 0049-0053. | Potential model must calculate targets and obstacles: TP1 must have free space to at least 1R; obstacle before TP1 blocks or downgrades. | Entry, stop, TP1, nearest obstacle/resistance/support, RR. | implemented | `RiskManager` enforces TP1 ≥ 1R; `BreakoutEatThrough` also pre-filters rounded TP1 below 1R and `nearest_resistance/support` or recent blocking `DensityDetected`/`LevelFormed` obstacle before TP1. | Keep tests for `PotentialObstacleBeforeTp1`, strategy `PotentialTp1BelowOneR`, and RiskManager `PoorRewardRisk`. |
| TP-01 | `ПЛАН СДЕЛКИ` lines 0001-0018: plan starts from chart/formation/levels/volume. | Every plan must trace chart context/formation/volume evidence, or mark missing data as downgrade/block. | FeatureFrame, signals, reason/evidence. | implemented | `TradePlan` stores `reason`, `evidence`, `frame_at_entry`; docs need explicit schema. | Document schema and persistence expectations. |
| TP-02 | `ПЛАН СДЕЛКИ` lines 0019-0031: order book state includes bid/ask fill, MM/robots, densities, dense/thin book. | Trade plan must record order-book entry/stop/obstacle evidence; unavailable visual categories must not be invented. | Density/iceberg signals, depth features; explicit detector payloads for robots/MM if ever added. | missing | Plans store density/eating evidence and frame depths, but no stable schema text for supported vs unsupported order-book evidence; no MM/robot identity model. | Document supported vs ambiguous/live-only fields. |
| TP-03 | `ПЛАН СДЕЛКИ` lines 0032-0054: tape speed/print size/contra prints/driver/news are core plan evidence. | Trade plan evidence must include tape and leader context; news/funding remain risk gates. | Tape signals, leader features, external blackout flags. | implemented | Breakout evidence includes `DensityEating` and `TapeBurst`; risk docs cover news/funding blackouts. | Document required evidence names per strategy. |
| TP-04 | `ПЛАН СДЕЛКИ` lines 0056-0061: risk per trade/day, working size, leverage, R:R, adding/closing, potential. | Plan exposes entry, stop, TP, R:R, sizing/risk basis and management criteria. | Entry/stop/TP fields, `risk_usd`, timeouts, TP ratio. | implemented | `TradePlan.hpp` has entry/stop/tp1/tp2/tp1_size_ratio/size/risk/timeouts; `RiskManager` computes sizing and RR. | Document persisted vs runtime fields; no schema extension needed unless Step 3 finds code gap. |
| TP-05 | `ПЛАН СДЕЛКИ` lines 0062-0065: footprint confirms eaten densities and total candle volume. | Footprint confirmation is desirable but only implemented if corresponding offline signal exists. | Footprint/eaten-density payload, candle volume. | live-only | No documented MetaScalp footprint API field in current strategy surface; `DensityEating` is available proxy. | Mark as live-only/proxy; do not invent fields. |
| TP-06 | `ПЛАН СДЕЛКИ` lines 0066-0072: breakout entry can be early, at level, after breakout, or combined. | Strategy entry type must be explicit and deterministic. | Entry mode, price, timeout. | implemented | `TradePlan.entry_type`, `entry_price`, `valid_until`; Breakout uses market/aggressive offset. | Document current Breakout mode and alternatives as not implemented unless strategy-specific. |
| TP-07 | `ПЛАН СДЕЛКИ` lines 0073-0086: strategy classes include bounce, reversal, density, false break, inefficiencies/manipulations. | Current TradePlan schema must apply consistently across all producers; unsupported patterns stay outside this task. | All TradePlan construction sites. | implemented | Bounce/Breakout/LeaderLag/FlushReversal produce the same `TradePlan` struct. | Audit in Step 3; update docs with universal invariants. |

## Live-only / ambiguous gaps not implemented in this task

- Market-maker/robot identity (`TP-02`) and spoofing intent (`ПЛАН СДЕЛКИ` lines 0021, 0027-0029) are not inferred from ticker strings or undocumented API fields.
- Full footprint/candle-volume semantics (`TP-05`) require a documented feed or detector; `DensityEating` is only a supported proxy.
- Higher-timeframe level visibility/V-shape/global formation scoring (`POT-03`) needs detector payloads before production gating.
- Full post-entry partial-management and new-obstacle monitoring (`WB-09`, `WB-10`) is only incompletely covered by existing TapeFade/LeaderMove/follow-through/timeouts.

## Reproducibility checks run

```bash
python3 <docx-extraction-snippet>
for f in reports/fn-002/docx-text/*.txt; do sed -n '1,80p' "$f"; done
```

Sanity phrases confirmed in extracted text: `нет заинтерисованности участников`, `Наличие поддержек`, `В боковиках хороших пробоев не бывает`, `ПЛАН СДЕЛКИ`, `Риск-прибыль`. Status sanity check: `grep -oE "\| (implemented|missing|ambiguous|live-only) \" reports/fn-002-weak-breakout-potential-trade-plan.md | wc -l` equals the number of rule rows, and `grep -nE "\| (nonconforming-status|docs-only) \" reports/fn-002-weak-breakout-potential-trade-plan.md` returns no matches.

## Step 2/3 documentation sanity checks

```bash
grep -R "WB-01\|POT-05\|TP-04\|WeakBreakoutNoSupportBehind\|PotentialObstacleBeforeTp1\|runtime-only" \
  docs/spec/STRATEGY_SOURCE_DIGEST.md docs/spec/STRATEGIES.md reports/fn-002-weak-breakout-potential-trade-plan.md
```

Result: exit code 0; confirmed source IDs, canonical reject names, potential/free-space labels, and persistence/runtime-only wording are present in source-of-truth docs and this report.


## Step 3 implementation notes

- `BreakoutEatThrough` now records stable pre-entry reject labels in `StrategyState.last_reject_reason` for implemented weak-breakout gates; missing/zero directional participation baseline rejects as `WeakBreakoutLowParticipation`.
- Density-eating progress uses canonical `SignalPayload.remaining_size` with a legacy fallback to `payload.size`; support-behind validation requires support-side density/iceberg to be directionally behind the breakout level, not merely within absolute distance; leader contra thresholds use `LeaderSignal` percent units directly (`abs(lag_pct)`).
- Formalized breakout thresholds are wired through `[strategies.breakout]` and numeric tier arrays are supported by `Config::get<std::vector<double>>`: `max_avg_spread_bps`, `tp1_r`, `support_search_range_bps`, `min_distance_from_best_bps`, `min_leader_correlation`, `leader_neutral_max_pct`, `leader_pre_entry_contra_pct` (LeaderSignal percent units: `abs(lag_pct)`), `min_tape_aggression`, `min_relative_volume`, and `max_resistance_cluster_ratio`; tier arrays are documented under `[strategies.breakout.tiers]`.

- Persistence update: active-trade `TradePlan` now round-trips TP2 and executor/strategy management fields (`no_progress_timeout_sec`, `post_entry_grace_sec`, `min_follow_through_bps`, leader/correlation exits, density stop anchor, approach count, trace id). Follow-through defaults are disabled (`0`) unless a strategy opts in.

- Step 4 coverage update: breakout tests include mirrored short-side valid plan and short obstacle-before-TP1 rejection; config tests cover numeric vector arrays used by `[strategies.breakout.tiers]`.
