# FN-004 — LeaderLag / FlushReversal source-status audit

Date: 2026-06-09  
Scope: `LeaderLag`, `FlushReversal`, their signal/config gates, and source traceability.

## Reproducibility

Manual DOCX text was extracted with the same stable paragraph-number convention used by FN-001/FN-002:

```bash
python3 - <<'PY'
import zipfile, re, pathlib, hashlib, tempfile
from xml.etree import ElementTree as ET
files=[
 'docs/Мануал по стратегии_начало.docx',
 'docs/Методическое_пособие_по_трейдингу_1 (2).docx',
 'docs/ПРОБОЙ.docx', 'docs/ПРОБОЙ СТРАТЕГИЯ.docx',
 'docs/ПЛАН СДЕЛКИ.docx', 'docs/ПОТЕНЦИАЛ.docx',
 'docs/крупные участники.docx', 'docs/неэфективности.docx']
ns={'w':'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
outdir=pathlib.Path(tempfile.mkdtemp(prefix='fn004-docx-text-'))
for f in files:
    p=pathlib.Path(f)
    data=p.read_bytes()
    xml=zipfile.ZipFile(p).read('word/document.xml')
    root=ET.fromstring(xml)
    paragraphs=[]
    for para in root.findall('.//w:p', ns):
        text=''.join(node.text or '' for node in para.iter() if node.tag == '{%s}t' % ns['w'])
        text=re.sub(r'[ \\t]+', ' ', text).strip()
        if text:
            paragraphs.append(text)
    out=outdir/(p.name+'.txt')
    with out.open('w', encoding='utf-8') as w:
        w.write(f'# Source: {f}\n# sha256: {hashlib.sha256(data).hexdigest()}\n')
        w.write('# method: python zipfile word/document.xml paragraph text-node extraction; whitespace collapsed\n')
        for i, text in enumerate(paragraphs, 1):
            w.write(f'{i:04d}: {text}\n')
print(outdir)
PY
```

The command writes to `/tmp/fn004-docx-text-*` for review only; generated extraction files are intentionally **not committed** to avoid bulk DOCX text in the repository. The audit cites stable paragraph identifiers produced by the command as `DOCX ¶0001-0002`; those identifiers can be reproduced by rerunning the command locally. Dependency outputs used: FN-001 (`reports/fn001/strategy_source_digest_bounce_breakout.md`), FN-002 (`reports/fn-002-weak-breakout-potential-trade-plan.md`), and FN-003 (`docs/spec/LARGE_PARTICIPANTS_AND_INEFFICIENCIES.md`) are all available and their tasks are Done.

## Status labels

Only these final labels are used:

- `production` — source-backed and fully offline-testable for the allowed execution mode.
- `gated` — source-backed but blocked from some modes by explicit checks.
- `phase-later` — valid concept requiring future feed/feature/test work.
- `documented-only` — recorded for traceability but not executable.
- `ambiguous` — source evidence conflicts or is insufficient.
- `pending-source-task` — blocked on FN-001/FN-002/FN-003 outputs.

## Preflight / dependency status

| Check | Result |
|---|---|
| Required DOCX/spec/source/test/config files | Present. |
| FN-001 | Done; digest and extracted lines available under `reports/fn001/`. |
| FN-002 | Done; formal report and extracted lines available under `reports/fn-002*`. |
| FN-003 | Done; large-participant/inefficiency spec available. |
| Initial build | Pre-existing infrastructure failure before edits: `./scripts/build.sh debug --tests` cannot generate/use Conan toolchain because Conan remote returns `403 Forbidden`; CMake then cannot find `build/debug/conan_toolchain.cmake` and Ninja. No source changes had been made. |

## LeaderLag audit

Final status: **`gated`**.

Allowed execution modes: paper and offline replay when synchronized leader/follower streams are present, correlation is above threshold, `LeaderMove` is fresh, density-on-path is absent, and instrument mapping is explicit. Live is allowed only under the same gates and with MetaScalp-documented market-data streams; spot/futures dislocation variants remain phase-later/live-gated until explicit multi-market identity and replay fixtures exist.

| Manual pattern | Required observations | Entry conditions | Invalidation | Stop anchor | TP/RR | Data dependency | Current implementation | Status | Reason | Required checks |
|---|---|---|---|---|---|---|---|---|---|---|
| Correlated instruments normally move together; dislocation creates opportunity. Source: `неэфективности.docx` 0004-0007 and 0096-0100. | Configured leader/follower mapping, fresh price streams, rolling correlation, lag magnitude. | `LeaderMove` with `lag_pct`, `correlation >= min_correlation`, signal age <= `lag_max_age`. | Reject stale/low-correlation signals; close if correlation drops below exit threshold or leader reverses. | Local swing behind follower price; for density-held variants, behind the blocking density. | Catch-up target based on fraction of lag; TP1 must pass general >=1R risk gate. | MetaScalp orderbook/trade streams for both instruments; no undocumented fields. | `LeaderLag` requires fresh `LeaderMove`, min correlation, spread, low own move, no density on path; stores post-entry correlation/leader exits. | `gated` | Core leader/follower lag concept is source-backed, but safe execution depends on explicit feeds/staleness/correlation gates. | Unit tests for happy path, low correlation, stale signal, density-on-path, post-entry leader/correlation exits. |
| BTC/market leader moved while coin is held by density/robot; coin catches down/up after support releases. Source: `неэфективности.docx` 0096. | Leader move, follower not moved, reason for lag (density/robot), release/eating signal. | Wait for release/eating of the blocking object before treating the lag as executable. | Leader reverses; density remains or path blocked. | Behind blocking density. | Catch-up only while leader move remains valid. | Leader feed plus orderbook density evidence; robot identity is unavailable. | Current code supports generic `LeaderMove` but actually rejects large density on path rather than trading density release. | `phase-later` | The manual density/robot-held variant needs release/eating semantics and robot-safe evidence, not actor identity. | Add future detector/strategy tests before enabling this subpattern. |
| Spot/futures dislocation: spot moves first, futures later catches after density eaten. Source: `неэфективности.docx` 0096-0107 and FN-003 §4.5/4.6. | Explicit spot↔futures instrument identity, basis/correlation model, both streams fresh, density on one venue. | No runtime entry until mapping and multi-feed replay exist. | Mapping missing/stale; basis normal; density absent; leader reverses. | Behind spot/futures density; manual wider stop / reduced size for futures-from-spot. | Must reflect wider stop and reduced size. | Requires explicit `(ConnectionId, MarketType, Ticker)` mapping; ticker-string normalization is forbidden. | Not implemented as standalone `SpotFuturesDislocation`; generic `LeaderLag` is insufficient. | `phase-later` | Source-backed but not safely testable/live-ready today. | Future task for explicit instrument identity, synchronized replay fixtures, config/risk tests. |
| Short-side / downside lag. Source: `неэфективности.docx` 0096 mentions BTC falling while coin held up; current docs note direction caveat. | Negative leader move, positive/negative correlation handled explicitly, follower lag. | Must not be silently enabled if sign/correlation semantics are incomplete. | Same as long plus correlation sign mismatch. | Swing high / density above. | Catch-up target. | Existing `LeaderLag` uses sign logic; docs previously called out short-side limitation. | `gated` | Executable only when tests prove the sign/correlation case; otherwise keep documented gap. | Dedicated short/direction-gated unit test or explicit documentation of unsupported variant. |

## FlushReversal audit

Final status: **`gated`** for paper/offline replay; **not live-grade by default**.

Allowed execution modes: paper and offline replay for the tape/book-only repeated-flush prototype. Live is blocked by default with `strategies.flushreversal.allow_live=false` and must require liquidation/open-interest/history gates before any live-grade claim.

| Manual pattern | Required observations | Entry conditions | Invalidation | Stop anchor | TP/RR | Data dependency | Current implementation | Status | Reason | Required checks |
|---|---|---|---|---|---|---|---|---|---|---|
| Repeated flushes: do not trade first flush; after >1% move place limits from 50% of flush range to max/min and wait minutes-hours for repeat. Source: `неэфективности.docx` 0052-0063; FN-003 §4.4. | At least two `TapeFlush` events in same window/range, strong level break, return/reversal, spread sane. | `LevelBreak` + recent `TapeFlush` count >= `min_flush_count` + volume fade + price reversal. | Single flush; no return; spread too wide; continuation burst/eating. | Local extremum beyond flush band / beyond broken level. | Reversal TP; current code uses 2R default. | MetaScalp `trade_update`/book/level signals; no liquidation feed needed for paper prototype. | `FlushReversal` requires repeated flush count, level strength, fade, reversal; rejects continuation `TapeBurst`/`DensityEating`. | `gated` | Source-backed and offline-testable for paper/replay, but it is not live-grade without external confirmations. | Tests for single-flush rejection, repeated-flush plan, continuation invalidations, allow_live default false. |
| Liquidation flushes after 10–15% move over 1–3 five-minute candles; close immediately if filled. Source: `неэфективности.docx` 0079-0092. | Liquidation feed, impulse context, OI/liquidation confirmation, very fast exit. | Live entry only on `LiquidationFlush`, not plain `TapeFlush`. | No liquidation/OI confirmation; stale external feed; no immediate exit path. | Extremum of liquidation band. | Immediate/short-hold exits, not trend holding. | External Binance/Bybit liquidation/OI feeds; not in MetaScalp contract. | `LiquidationDetector` is planned; `FlushReversal` code does not consume `LiquidationFlush`. | `phase-later` | Source-backed but requires unimplemented external feeds and tests. | T5-FLUSH implementation, replay/live fixtures, strategy-level live gate tests. |
| Plain `TapeFlush` is a noisy print if not repeated/confirmed. Source: FN-003 §4.4; Signal spec §9. | Distinguish `TapeFlush` from `LiquidationFlush` and from continuation `TapeBurst`/`DensityEating`. | Plain single `TapeFlush` must not create a live-grade plan. | Continuation signals invalidate. | Same as above. | Same as above. | Current MetaScalp can provide trades but not force-order/OI data. | Code uses repeated `TapeFlush` for prototype only. | `gated` | Safe only when explicitly documented as paper/offline prototype. | Spec-consistency test must assert not live-grade/default `allow_live=false`. |

## MetaScalp/API assumptions

No new MetaScalp endpoints or fields are assumed. `LeaderLag` uses existing market-data-derived `LeaderMove`; spot/futures identity must preserve `ConnectionId`, market type, and exact ticker per `METASCALP_API_CONTRACT.md`. `FlushReversal` liquidation/OI confirmation is explicitly external/phase-later and not part of the MetaScalp contract.

## Conflict resolution

Targeted grep terms: `LeaderLag`, `FlushReversal`, `LiquidationFlush`, `allow_live`, `LeaderMove` across docs/spec, config, core strategy source, and unit tests.

| Location / conflicting wording | Chosen final wording | Why |
|---|---|---|
| `STRATEGIES.md §3` presents `LeaderLag` as core but lacks a status block. | `LeaderLag` status is `gated`: source-backed leader/follower lag with mandatory correlation/staleness/path gates; spot/futures variant phase-later. | Prevents reading generic leader logic as authorization for unsafe spot/futures or robot-held variants. |
| `STRATEGIES.md §4` says `FlushReversal` is paper prototype / T5 live, but matrix rows show checkmarks in market regimes. | `FlushReversal` status is `gated` for paper/offline only; live default is disabled. | Checkmarks can be misread as live-ready; final wording distinguishes allowed mode from pattern applicability. |
| `STRATEGIES.md §4.5` says paper/backtest or explicit `allow_live=true` after manual confirmation. | Live remains disabled by default; `allow_live=true` alone is insufficient for live-grade unless liquidation/OI/history gates are implemented and tested. | Manual confirmation cannot substitute for automated live gates. |
| `STRATEGIES.md §8` labels `LeaderLag` as `Core` and `FlushReversal` as `Paper prototype / T5 live`. | Matrix labels become `gated` and `gated paper/offline; live phase-later`. | Uses the required final labels and avoids unsupported readiness claims. |
| `SIGNAL_DETECTION.md §9` states `LiquidationDetector` planned and `FlushReversal` paper prototype without liquidation hard gate. | Keep this; add explicit live gate wording that plain `TapeFlush` must not satisfy live `LiquidationFlush`. | Aligns with source and API constraints. |
| `TASK_SPECS.md T3-LEADERLAG` asks for all C* conditions. | Mark deliverable as gated and require source/status consistency tests. | T3 implementation can exist but remains constrained by staleness/correlation/feed gates. |
| `TASK_SPECS.md T5-FLUSH` describes live production with liquidation/OI. | Keep as phase-later acceptance target; current runtime must not claim it. | T5 is the proper home for external feed work. |
| `ARCHITECTURE.md` affinity example includes `[strategies.flushreversal] allow_live=false`. | Keep and mirror in config/runtime tests. | Existing architecture already matches safe default. |
| `config/config.example.toml` has `allow_live=false` without enough strategy-level explanation. | Add comments that runtime strategy remains paper/offline unless T5 gates are present; load/test default. | Makes safe default auditable. |

## Follow-up tasks

- Implement explicit spot/futures/leader instrument identity for dislocation variants: acceptance requires `(ConnectionId, MarketType, exact Ticker)` mapping, synchronized replay fixture, and tests rejecting string-only aliases.
- Implement T5 liquidation/open-interest gates for live `FlushReversal`: acceptance requires `LiquidationFlush` detector, OI/liquidation fixture, and tests proving plain `TapeFlush` cannot satisfy live mode.
