
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
  spread_bps: number;
  buy_vol_5s: number;
  sell_vol_5s: number;
  volatility_1min_bps: number;
  tape_aggression: number;
  leader_correlation: number;
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
  side: 'LONG' | 'SHORT';
  entry_price: number;
  mark_price: number;
  pnl_usd: number;
  pnl_pct: number;
  status: 'OPEN' | 'CLOSED';
  time: string;
  strategy: string;
}

export interface JournalEntry {
  id: string;
  ticker: string;
  side: 'LONG' | 'SHORT';
  entryPrice: number;
  exitPrice: number;
  pnlUsd: number;
  pnlPct: number;
  size: number;
  entryTimeMs: number;
  exitTimeMs: number;
  strategy: string;
}
