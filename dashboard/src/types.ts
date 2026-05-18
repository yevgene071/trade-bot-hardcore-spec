
export enum StrategyReadyState {
  Cold = 0,
  Warming = 1,
  Ready = 2,
  Planning = 3,
  Trading = 4,
  Cooldown = 5,
}

export interface StrategyCondition {
  name: string;
  current: number;
  target: number;
  met: boolean;
  unit: string;
}

export interface StrategyState {
  ticker: string;
  strategy_name: string;
  ready_state: StrategyReadyState;
  readiness_pct: number;
  conditions: StrategyCondition[];
  last_reject_reason?: string;
  seconds_since_last_reject?: number;
  signals_last_60s?: number;
}

export interface ObLevel {
  price: number;
  size: number;
}

export interface ChartPoint {
  ts_unix_ms: number;
  mid: number;
  best_bid: number;
  best_ask: number;
  spread_bps: number;
  buy_vol_5s: number;
  sell_vol_5s: number;
  volatility_1min_bps: number;
  tape_aggression: number;
  leader_change_1s: number;
  leader_correlation: number;
  leader_lag_ms: number;
  imbalance: number;
  prints_per_sec: number;
}

export interface DensityColumn {
  ts_unix_ms: number;
  lo: number;
  hi: number;
  bins: number[]; // normalized 0..255, sqrt-contrast, low→high price
}

export interface IcebergEvent {
  ts_ms: number;
  price: number;
  side: string; // "BID" or "ASK"
  hidden_size: number;
}

export interface TickerInfo {
  ticker: string;
  mark_price: number;
  boosted: boolean;
  strategies: string[];
}

export interface Trade {
  id: string;
  ticker: string;
  side: 'LONG' | 'SHORT' | 'UNKNOWN'; // FUNC-01: store emits 'UNKNOWN' when plan.side absent
  entry_price: number;
  mark_price: number;
  pnl_usd: number;
  pnl_pct: number;
  status: 'OPEN' | 'CLOSED';
  time: string;
  strategy: string;
  // Position levels — emitted alongside the snake_case fields for the
  // AdvancedChart L1 layer (camelCase, all optional → backward compatible).
  entryPrice?: number;
  unrealizedPnl?: number;
  executedSize?: number;
  sizeCoin?: number;
  stopLoss?: number;
  takeProfit?: number;
}

export interface JournalEntry {
  id: string;
  ticker: string;
  side: 'LONG' | 'SHORT' | 'UNKNOWN'; // FUNC-01
  entryPrice: number;
  exitPrice: number;
  pnlUsd: number;
  pnlPct: number;
  size: number;
  entryTimeMs: number;
  exitTimeMs: number;
  strategy: string;
  // Used by AdvancedChart journal markers (optional → backward compatible).
  tsMs?: number;          // exit timestamp (ts_unix_ms)
  entryTsMs?: number;     // entry timestamp
  causeOfExit?: string;   // 'TP' | 'SL' | 'SIGNAL' | 'TIME' | ...
}
