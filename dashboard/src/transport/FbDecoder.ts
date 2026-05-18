import * as flatbuffers from 'flatbuffers';
import { DashboardState } from '../generated/trade-bot-dashboard/dashboard-state.js';
import { BookLevel } from '../generated/trade-bot-dashboard/book-level.js';
import { EquityPoint } from '../generated/trade-bot-dashboard/equity-point.js';

// Decodes a FlatBuffers binary frame into the same plain-object shape
// that the JSON WebSocket path produces, so applyServerState works unchanged.
export function decodeFlatBuffer(bytes: Uint8Array): Record<string, unknown> {
  const buf = new flatbuffers.ByteBuffer(bytes);
  const s = DashboardState.getRootAsDashboardState(buf);
  const isFull = s.isFullUpdate();

  // signal_counts: encoded as parallel key / value arrays in the schema
  const signal_counts: Record<string, number> = {};
  for (let i = 0; i < s.signalCountKeysLength(); i++) {
    const k = s.signalCountKeys(i);
    if (k !== null) signal_counts[k] = s.signalCountValues(i) ?? 0;
  }

  const open_trades: unknown[] = [];
  for (let i = 0; i < s.openTradesLength(); i++) {
    const t = s.openTrades(i);
    if (!t) continue;
    const p = t.plan();
    open_trades.push({
      plan: {
        ticker: p?.ticker() ?? '',
        side: p?.side() ?? 0,
        strategy_name: p?.strategyName() ?? '',
        stop_price: p?.stopPrice() ?? 0,
        tp1_price: p?.tp1Price() ?? 0,
      },
      executed_size: t.executedSize(),
      avg_entry_price: t.avgEntryPrice(),
      unrealized_pnl: t.unrealizedPnl(),
    });
  }

  const recent_journal: unknown[] = [];
  if (isFull) {
    for (let i = 0; i < s.recentJournalLength(); i++) {
      const e = s.recentJournal(i);
      if (!e) continue;
      const p = e.plan();
      recent_journal.push({
        plan: {
          ticker: p?.ticker() ?? '',
          strategy_name: p?.strategyName() ?? '',
          size_coin: p?.sizeCoin() ?? 0,
          side: p?.side() ?? 0,
          entry_price: p?.entryPrice() ?? 0,
        },
        pnl_usd: e.pnlUsd(),
        exit_price: e.exitPrice(),
        cause_of_exit: e.causeOfExit() ?? '',
        ts_unix_ms: Number(e.tsUnixMs()),
      });
    }
  }

  const ms = s.metascalp();
  const metascalp = {
    connected: ms?.connected() ?? false,
    latency_ms: ms?.latencyMs() ?? 0,
    connection_name: ms?.connectionName() ?? '',
  };

  const universe: unknown[] = [];
  for (let i = 0; i < s.universeRowsLength(); i++) {
    const r = s.universeRows(i);
    if (!r) continue;
    const strategies: string[] = [];
    for (let j = 0; j < r.strategiesLength(); j++) {
      strategies.push(r.strategies(j) ?? '');
    }
    universe.push({
      ticker: r.ticker() ?? '',
      strategies,
      boosted: r.boosted(),
      mark_price: r.markPrice(),
    });
  }

  const recent_signals: unknown[] = [];
  for (let i = 0; i < s.recentSignalsLength(); i++) {
    const sg = s.recentSignals(i);
    if (!sg) continue;
    recent_signals.push({
      kind: sg.kind() ?? '',
      ticker: sg.ticker() ?? '',
      price: sg.price(),
      confidence: sg.confidence(),
      time_str: sg.timeStr() ?? '',
      side: sg.side() ?? '',
    });
  }

  const strategy_stats: unknown[] = [];
  if (isFull) {
    for (let i = 0; i < s.strategyStatsLength(); i++) {
      const st = s.strategyStats(i);
      if (!st) continue;
      strategy_stats.push({
        name: st.name() ?? '',
        total_trades: st.totalTrades(),
        wins: st.wins(),
        losses: st.losses(),
        total_pnl: st.totalPnl(),
        best_pnl: st.bestPnl(),
        worst_pnl: st.worstPnl(),
        gross_profit: st.grossProfit(),
        gross_loss: st.grossLoss(),
      });
    }
  }

  const funding_info: unknown[] = [];
  if (isFull) {
    for (let i = 0; i < s.fundingInfoLength(); i++) {
      const fi = s.fundingInfo(i);
      if (!fi) continue;
      funding_info.push({
        ticker: fi.ticker() ?? '',
        rate: fi.rate(),
        next_funding_unix: Number(fi.nextFundingUnix()),
      });
    }
  }

  const rk = s.risk();
  const risk = {
    margin_used_pct: rk?.marginUsedPct() ?? 0,
    exposure_pct: rk?.exposurePct() ?? 0,
    total_trades_today: rk?.totalTradesToday() ?? 0,
    consecutive_losses: rk?.consecutiveLosses() ?? 0,
    daily_pnl_pct: rk?.dailyPnlPct() ?? 0,
    current_drawdown_pct: rk?.currentDrawdownPct() ?? 0,
    max_positions: rk?.maxPositions() ?? 0,
  };

  const acc = s.account();
  const account = {
    equity_usd: acc?.equityUsd() ?? 0,
    realized_pnl_today_usd: acc?.realizedPnlTodayUsd() ?? 0,
    unrealized_pnl_usd: acc?.unrealizedPnlUsd() ?? 0,
    free_balance_usd: acc?.freeBalanceUsd() ?? 0,
    starting_equity_usd: acc?.startingEquityUsd() ?? 0,
  };

  const iceberg_events: unknown[] = [];
  for (let i = 0; i < s.icebergEventsLength(); i++) {
    const ev = s.icebergEvents(i);
    if (!ev) continue;
    iceberg_events.push({
      ts_ms: Number(ev.tsMs()),
      price: ev.price(),
      side: ev.side() ?? '',
      hidden_size: ev.hiddenSize(),
    });
  }

  const strategy_states: unknown[] = [];
  if (isFull) {
    for (let i = 0; i < s.strategyStatesLength(); i++) {
      const ss = s.strategyStates(i);
      if (!ss) continue;
      const conditions: unknown[] = [];
      for (let j = 0; j < ss.conditionsLength(); j++) {
        const c = ss.conditions(j);
        if (!c) continue;
        conditions.push({
          name: c.name() ?? '',
          current: c.current(),
          target: c.target(),
          met: c.met(),
          unit: c.unit() ?? '',
        });
      }
      strategy_states.push({
        ticker: ss.ticker() ?? '',
        strategy_name: ss.strategyName() ?? '',
        ready_state: ss.readyState(),
        readiness_pct: ss.readinessPct(),
        conditions,
        last_reject_reason: ss.lastRejectReason() ?? '',
        seconds_since_last_reject: ss.secondsSinceLastReject(),
        signals_last_60s: ss.signalsLast60s(),
      });
    }
  }

  const chart_history: unknown[] = [];
  if (isFull) {
    for (let i = 0; i < s.chartHistoryLength(); i++) {
      const pt = s.chartHistory(i);
      if (!pt) continue;
      chart_history.push({
        ts_unix_ms: Number(pt.tsUnixMs()),
        mid: pt.mid(),
        best_bid: pt.bestBid(),
        best_ask: pt.bestAsk(),
        spread_bps: pt.spreadBps(),
        buy_vol_5s: pt.buyVol5s(),
        sell_vol_5s: pt.sellVol5s(),
        volatility_1min_bps: pt.volatility1minBps(),
        tape_aggression: pt.tapeAggression(),
        leader_change_1s: pt.leaderChange1s(),
        leader_correlation: pt.leaderCorrelation(),
        leader_lag_ms: pt.leaderLagMs(),
        imbalance: pt.imbalance(),
        prints_per_sec: pt.printsPerSec(),
      });
    }
  }

  const density_history: unknown[] = [];
  if (isFull) {
    for (let i = 0; i < s.densityHistoryLength(); i++) {
      const col = s.densityHistory(i);
      if (!col) continue;
      const binsArr = col.binsArray();
      density_history.push({
        ts_unix_ms: Number(col.tsUnixMs()),
        lo: col.lo(),
        hi: col.hi(),
        bins: binsArr ? Array.from(binsArr) : [],
      });
    }
  }

  // Structs: reuse a single object per array to avoid allocations
  const bids_top20: unknown[] = [];
  const asks_top20: unknown[] = [];
  if (isFull) {
    const lvObj = new BookLevel();
    for (let i = 0; i < s.bidsTop20Length(); i++) {
      const lv = s.bidsTop20(i, lvObj);
      if (lv) bids_top20.push({ price: lv.price(), size: lv.size() });
    }
    for (let i = 0; i < s.asksTop20Length(); i++) {
      const lv = s.asksTop20(i, lvObj);
      if (lv) asks_top20.push({ price: lv.price(), size: lv.size() });
    }
  }

  const equity_history: unknown[] = [];
  if (isFull) {
    const epObj = new EquityPoint();
    for (let i = 0; i < s.equityHistoryLength(); i++) {
      const pt = s.equityHistory(i, epObj);
      if (pt) equity_history.push({ ts: Number(pt.ts()), equity: pt.equity() });
    }
  }

  const result: Record<string, unknown> = {
    version: s.version() ?? '',
    state_gen: Number(s.stateGen()),
    is_full_update: isFull,
    server_time_unix: Number(s.serverTimeUnix()),
    kill_switch_active: s.killSwitchActive(),
    recorder_active: s.recorderActive(),
    recorder_path: s.recorderPath() ?? '',
    account,
    signal_counts,
    open_trades,
    metascalp,
    universe,
    recent_signals,
    risk,
    iceberg_events,
    ob_mid: s.obMid(),
    ob_spread_bps: s.obSpreadBps(),
    ob_imbalance: s.obImbalance(),
    selected_ticker: s.selectedTicker() ?? '',
  };

  if (isFull) {
    result.recent_journal = recent_journal;
    result.strategy_stats = strategy_stats;
    result.funding_info = funding_info;
    result.strategy_states = strategy_states;
    result.chart_history = chart_history;
    result.density_history = density_history;
    result.bids_top20 = bids_top20;
    result.asks_top20 = asks_top20;
    result.equity_history = equity_history;
  }

  return result;
}
