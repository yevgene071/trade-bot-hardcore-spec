import { create } from 'zustand';
import { Trade, StrategyState, StrategyReadyState, JournalEntry, ObLevel } from '../types';

export interface Signal {
  id: number;
  type: string;
  ticker: string;
  age: string;
  conf: number;
  side: 'BUY' | 'SELL' | 'LONG' | 'SHORT';
  text: string;
  value: string;
  count?: number; 
  timestamp: number;
  price?: number;
}

interface TradeStore {
  activeTicker: string;
  setActiveTicker: (ticker: string) => void;
  killSwitchActive: boolean;
  toggleKillSwitch: () => void;
  latency: number;
  setLatency: (ms: number) => void;
  equity: number;
  sessionPnL: number;
  sessionPnLPct: number;
  marginUsedPct: number;
  dailyDrawdownPct: number;
  
  tickerPrices: Record<string, number>;
  updatePrices: (prices: Record<string, number>) => void;
  
  trades: Trade[];
  activeOrders: { id: string, price: number, side: 'BUY' | 'SELL', ticker: string }[];
  addOrder: (order: { price: number, side: 'BUY' | 'SELL', ticker: string }) => void;
  removeOrder: (id: string) => void;
  signals: Signal[];
  addSignal: (signal: Signal) => void;
  
  strategyStates: StrategyState[];
  setStrategyStates: (states: StrategyState[]) => void;

  // Real-time data from server
  orderbook: { bids: ObLevel[]; asks: ObLevel[]; mid: number; spreadBps: number; imbalance: number };
  journalEntries: JournalEntry[];
  equityHistory: { ts: number; val: number }[];
  spreadHistory: { ts: number; val: number }[];
  aggressionHistory: { ts: number; val: number }[];
  volumeHistory: { ts: number; buy: number; sell: number }[];
  wsConnected: boolean;

  applyServerState: (data: any) => void;
  setWsConnected: (v: boolean) => void;

}

export const useTradeStore = create<TradeStore>((set) => ({
  activeTicker: 'BINANCE:BTCUSDT.p',
  setActiveTicker: (ticker: string) => set({ activeTicker: ticker }),

  killSwitchActive: false,
  toggleKillSwitch: () => set(state => ({ killSwitchActive: !state.killSwitchActive })),
  latency: 0,
  setLatency: (ms) => set({ latency: ms }),
  equity: 0,
  sessionPnL: 0,
  sessionPnLPct: 0,
  marginUsedPct: 0,
  dailyDrawdownPct: 0,

  tickerPrices: {},
  updatePrices: (prices) => set({ tickerPrices: prices }),

  trades: [],

  activeOrders: [],
  addOrder: (o) => set(s => ({ activeOrders: [...s.activeOrders, { ...o, id: Math.random().toString() }] })),
  removeOrder: (id) => set(s => ({ activeOrders: s.activeOrders.filter(x => x.id !== id) })),

  signals: [],

  addSignal: (signal) => set(state => {
    // Aggregate signals logic
    const existing = state.signals.find(s => s.ticker === signal.ticker && s.type === signal.type && (signal.timestamp - s.timestamp < 1000));
    if (existing) {
       return {
         signals: state.signals.map(s => s.id === existing.id ? { ...s, count: (s.count || 1) + 1, conf: Math.max(s.conf, signal.conf), timestamp: signal.timestamp } : s)
       };
    }
    return { signals: [signal, ...state.signals].slice(0, 50) };
  }),
  
  strategyStates: [],
  setStrategyStates: (states) => set({ strategyStates: states }),

  orderbook: { bids: [], asks: [], mid: 0, spreadBps: 0, imbalance: 0 },
  journalEntries: [],
  equityHistory: [],
  spreadHistory: [],
  aggressionHistory: [],
  volumeHistory: [],
  wsConnected: false,
  setWsConnected: (v) => set({ wsConnected: v }),

  applyServerState: (data: any) => set(state => {
    const acc = data.account || {};
    const risk = data.risk || {};

    const newPrices = { ...state.tickerPrices };
    if (data.ob_mid && data.selected_ticker) {
      newPrices[data.selected_ticker] = data.ob_mid;
    }

    const equityHistory = (data.equity_history || []).map((e: any) => ({ ts: e.ts, val: e.equity }));

    const chartHistory = data.chart_history || [];
    const spreadHistory = chartHistory.map((c: any) => ({ ts: c.ts, val: c.spread_bps }));
    const aggressionHistory = chartHistory.map((c: any) => ({ ts: c.ts, val: c.tape_aggression }));
    const volumeHistory = chartHistory.map((c: any) => ({ ts: c.ts, buy: c.buy_vol_5s, sell: c.sell_vol_5s }));

    const journalEntries: JournalEntry[] = (data.recent_journal || []).map((j: any) => ({
      id: j.id || String(j.entry_time_ms),
      ticker: j.ticker,
      side: j.side,
      entryPrice: j.entry_price,
      exitPrice: j.exit_price,
      pnlUsd: j.pnl_usd,
      pnlPct: j.pnl_pct,
      size: j.size,
      entryTimeMs: j.entry_time_ms,
      exitTimeMs: j.exit_time_ms,
      strategy: j.strategy,
    }));

    const newSignals = (data.recent_signals || []).map((s: any, i: number) => ({
      id: i,
      type: s.kind,
      ticker: s.ticker,
      age: s.time_str,
      conf: s.confidence,
      side: s.confidence > 0 ? 'BUY' : 'SELL',
      text: s.kind,
      value: `$${Math.round(Math.abs(s.price || 0))}`,
      count: 1,
      timestamp: Date.now(),
      price: s.price,
    }));

    return {
      killSwitchActive: data.kill_switch_active ?? state.killSwitchActive,
      equity: acc.equity_usd ?? state.equity,
      sessionPnL: acc.realized_pnl_today_usd ?? state.sessionPnL,
      sessionPnLPct: acc.realized_pnl_today_usd && acc.starting_equity_usd
        ? (acc.realized_pnl_today_usd / acc.starting_equity_usd) * 100
        : state.sessionPnLPct,
      marginUsedPct: risk.margin_used_pct ?? state.marginUsedPct,
      dailyDrawdownPct: risk.current_drawdown_pct ?? state.dailyDrawdownPct,
      latency: data.metascalp?.latency_ms ?? state.latency,
      tickerPrices: newPrices,
      trades: data.open_trades ?? state.trades,
      signals: newSignals.length ? newSignals : state.signals,
      strategyStates: data.strategy_states ?? state.strategyStates,
      orderbook: {
        bids: data.bids_top20 || [],
        asks: data.asks_top20 || [],
        mid: data.ob_mid || 0,
        spreadBps: data.ob_spread_bps || 0,
        imbalance: data.ob_imbalance || 0,
      },
      journalEntries,
      equityHistory: equityHistory.length ? equityHistory : state.equityHistory,
      spreadHistory: spreadHistory.length ? spreadHistory : state.spreadHistory,
      aggressionHistory: aggressionHistory.length ? aggressionHistory : state.aggressionHistory,
      volumeHistory: volumeHistory.length ? volumeHistory : state.volumeHistory,
      activeTicker: data.selected_ticker || state.activeTicker,
    };
  }),

}));
