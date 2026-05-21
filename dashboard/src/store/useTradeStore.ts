import { create } from 'zustand';
import { Trade, StrategyState, StrategyReadyState, JournalEntry, ObLevel, ChartPoint, IcebergEvent, DensityColumn } from '../types';
import { marketDataService } from '../services/MarketDataService';
import { normalizeTicker } from '../utils/tickerUtils';

const MAX_CHART_POINTS = 600;

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
  unrealizedPnl: number;
  freeBalance: number;
  recorderActive: boolean;
  recorderPath: string;
  
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

  risk: { exposurePct: number; totalTradesToday: number; consecutiveLosses: number };

  // Real-time data from server
  orderbook: { bids: ObLevel[]; asks: ObLevel[]; mid: number; spreadBps: number; imbalance: number };
  journalEntries: JournalEntry[];
  equityHistory: { ts: number; val: number }[];
  spreadHistory: { ts: number; val: number }[];
  aggressionHistory: { ts: number; val: number }[];
  volumeHistory: { ts: number; buy: number; sell: number }[];
  chartHistory: ChartPoint[];
  densityHistory: DensityColumn[];
  icebergEvents: IcebergEvent[];
  universeTickers: string[];
  wsStatus: 'connected' | 'disconnected' | 'connecting' | 'reconnecting';
  reconnectAttempt: number;
  lastReconnectAt: number | null; // epoch ms of last reconnect start
  wsMsgCount: number; // total messages received in current session
  lastStateGen: number; // server-side generation counter, skip re-apply if unchanged

  applyServerState: (data: any) => void;
  setWsStatus: (status: 'connected' | 'disconnected' | 'connecting' | 'reconnecting') => void;
  setReconnectAttempt: (n: number) => void;
  setLastReconnectAt: (ts: number | null) => void;
  incrementWsMsgCount: () => void;
  batchAddWsMsgCount: (n: number) => void;

}

export const useTradeStore = create<TradeStore>((set) => ({
  activeTicker: 'BTC_USDT', // D1-FIX: canonical format, not 'BINANCE:BTCUSDT.p'
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
  risk: { exposurePct: 0, totalTradesToday: 0, consecutiveLosses: 0 },
  unrealizedPnl: 0,
  freeBalance: 0,
  recorderActive: false,
  recorderPath: '',

  tickerPrices: {},
  updatePrices: (prices) => set({ tickerPrices: prices }),

  trades: [],

  activeOrders: [],
  addOrder: (o) => set(s => ({ activeOrders: [...s.activeOrders, { ...o, id: Math.random().toString() }] })),
  removeOrder: (id) => set(s => ({ activeOrders: s.activeOrders.filter(x => x.id !== id) })),

  signals: [],

  addSignal: (signal) => set(state => {
    // D5-FIX: Reduce aggregation window to 300ms to catch rapid-fire signals
    // E3-FIX: Normalize tickers for dedup to avoid format-dependent duplicates
    const normSigTicker = normalizeTicker(signal.ticker);
    const existing = state.signals.find(s => normalizeTicker(s.ticker) === normSigTicker && s.type === signal.type && (signal.timestamp - s.timestamp < 300));
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
  chartHistory: [],
  densityHistory: [],
  icebergEvents: [],
  universeTickers: [],
  wsStatus: 'disconnected',
  reconnectAttempt: 0,
  lastReconnectAt: null,
  wsMsgCount: 0,
  lastStateGen: -1, // MATH-09: -1 so a server's first frame (state_gen:0) is not skipped
  setWsStatus: (status) => set(state => {
    // Reset message counter on each new connection attempt
    if (status === 'connecting') return { wsStatus: status, wsMsgCount: 0 };
    return { wsStatus: status };
  }),
  setReconnectAttempt: (n) => set({ reconnectAttempt: n }),
  setLastReconnectAt: (ts) => set({ lastReconnectAt: ts }),
  incrementWsMsgCount: () => set(s => ({ wsMsgCount: s.wsMsgCount + 1 })),
  batchAddWsMsgCount: (n: number) => set(s => ({ wsMsgCount: s.wsMsgCount + n })),

  applyServerState: (data: any) => set(state => {
    // WS-02: skip if state_gen hasn't changed — prevents unnecessary re-renders
    // when the server sends identical state every 100ms tick.
    // STALE-FIX: Always update if data is older than 2s to prevent freeze
    const now = Date.now();
    const lastUpdate = state.chartHistory[state.chartHistory.length - 1]?.ts_unix_ms ?? 0;
    const isStale = lastUpdate > 0 && (now - lastUpdate) > 2000;
    
    if (!isStale && data.state_gen !== undefined && data.state_gen === state.lastStateGen) {
      return {}; // empty merge → no re-render
    }
    const acc = data.account || {};
    const risk = data.risk || {};

    const newPrices = { ...state.tickerPrices };
    if (data.ob_mid && data.selected_ticker) {
      newPrices[normalizeTicker(data.selected_ticker)] = data.ob_mid;
    }
    const universeTickers: string[] = data.universe
      ? (data.universe as any[]).map((r: any) => { 
          const t = normalizeTicker(r.ticker);
          newPrices[t] = r.mark_price; 
          return t; 
        })
      : state.universeTickers;

    const equityHistory = (data.equity_history || []).map((e: any) => ({ ts: e.ts, val: e.equity }));

    // D1-FIX: Build map for O(1) dedup during merge, prevents race condition
    let mergedChartHistory = state.chartHistory;
    let spreadHistory = state.spreadHistory;
    let aggressionHistory = state.aggressionHistory;
    let volumeHistory = state.volumeHistory;

    if (data.chart_history && (data.chart_history as any[]).length > 0) {
      // PERF-02: Push to mutable buffer instead of creating new arrays
      const ticker = data.selected_ticker || state.activeTicker;
      const buffer = marketDataService.getBuffer(ticker);
      
      // Push new points to circular buffer (zero allocations)
      for (const p of (data.chart_history as ChartPoint[])) {
        if (p.ts_unix_ms > 0) {
          buffer.chart.push(p);
        }
      }
      
      // Keep legacy array for components not yet migrated
      mergedChartHistory = buffer.chart.toArray();
      
      // DEV-ONLY: Verify server sends sorted data
      if ((import.meta as any).env?.DEV && mergedChartHistory.length > 1) {
        const isOrdered = mergedChartHistory.every((p, i, arr) => 
          i === 0 || p.ts_unix_ms >= arr[i - 1].ts_unix_ms
        );
        if (!isOrdered) {
          console.error('[PERF-01] Server sent unordered chart_history! This will cause visual glitches.');
        }
      }
      // Only recompute derived histories when chart data actually arrived,
      // not on every 100ms fast tick (perf: avoids 600-pt map on each tick).
      spreadHistory = mergedChartHistory.map((c: ChartPoint) => ({ ts: c.ts_unix_ms, val: c.spread_bps }));
      aggressionHistory = mergedChartHistory.map((c: ChartPoint) => ({ ts: c.ts_unix_ms, val: c.tape_aggression }));
      volumeHistory = mergedChartHistory.map((c: ChartPoint) => ({ ts: c.ts_unix_ms, buy: c.buy_vol_5s, sell: c.sell_vol_5s }));
    }

    const trades = (data.open_trades || []).map((t: any) => {
      const side = t.plan?.side === 0 ? 'LONG' : (t.plan?.side === 1 ? 'SHORT' : 'UNKNOWN');
      const entry = t.avg_entry_price || 0;
      const pnl = t.unrealized_pnl || 0;
      return {
        id: t.plan?.ticker || Math.random().toString(),
        ticker: t.plan?.ticker || 'UNKNOWN',
        side,
        entry_price: entry,
        pnl_usd: pnl,
        strategy: t.plan?.strategy_name || 'Manual',
        mark_price: newPrices[t.plan?.ticker] || entry,
        // camelCase mirror + position levels for AdvancedChart L1 layer.
        // SL/TP come from plan.* and were previously dropped here.
        entryPrice: entry,
        unrealizedPnl: pnl,
        executedSize: t.executed_size,
        sizeCoin: t.plan?.size_coin,
        stopLoss: t.plan?.stop_price,
        takeProfit: t.plan?.tp1_price,
      };
    });

    // WS-02: only rebuild journal from server data if is_full_update is true (slow tick 1s).
    // On fast ticks (100ms), data.recent_journal is undefined — preserve existing journal.
    const journalEntries: JournalEntry[] = data.is_full_update !== false && data.recent_journal
      ? (data.recent_journal as any[]).map((j: any) => {
          const tsMs = j.ts_unix_ms;
          // D7-FIX: Detect if hold_ms is in seconds and convert to milliseconds
          let holdMs = j.hold_ms ?? 0;
          // If hold_ms > 10000 (10s) but trade looks short-lived, assume seconds
          if (holdMs > 10000 && j.pnl_pct && Math.abs(j.pnl_pct) < 0.5) {
            holdMs *= 1000; // convert seconds to milliseconds
          }
          const entryTsMs = j.entry_ts_unix_ms ?? j.plan?.entry_ts ?? (tsMs - holdMs);
          if ((import.meta as any).env?.DEV && j.exit_price && entryTsMs >= tsMs) {
            // eslint-disable-next-line no-console
            console.assert(false, `[journal] entryTsMs (${entryTsMs}) >= exitTsMs (${tsMs}) — hold_ms unit suspect`, j);
          }
          return {
            id: j.id || String(j.ts_unix_ms),
            ticker: j.plan?.ticker || j.ticker,
            side: j.plan?.side === 0 ? 'LONG' : (j.plan?.side === 1 ? 'SHORT' : j.side),
            entryPrice: j.plan?.entry_price || j.entry_price,
            exitPrice: j.exit_price,
            pnlUsd: j.pnl_usd,
            pnlPct: j.pnl_pct || 0,
            size: j.plan?.size_coin || j.size,
            entryTimeMs: j.entry_time_ms || j.ts_unix_ms,
            exitTimeMs: j.ts_unix_ms || j.exit_time_ms,
            strategy: j.plan?.strategy_name || j.strategy,
            // For AdvancedChart journal markers — previously dropped.
            tsMs,
            entryTsMs,
            causeOfExit: j.cause_of_exit,
          };
        })
      : state.journalEntries;

    // Stable identity so age timers keep counting instead of resetting to 0s
    // every full update. recent_signals is a rolling server list; a signal
    // that persists across ticks must keep its first-seen wall-clock time.
    const prevByKey = new Map<string, Signal>();
    for (const ps of state.signals) {
      prevByKey.set(`${ps.ticker}|${ps.type}|${ps.age}|${ps.price ?? ''}`, ps);
    }
    const signalNow = Date.now();
    const newSignals = (data.recent_signals || []).map((s: any) => {
      let mappedSide = 'BUY';
      if (typeof s.side === 'string') {
        const u = s.side.toUpperCase();
        if (u.includes('ASK') || u.includes('SELL')) mappedSide = 'SELL';
      } else {
        mappedSide = s.confidence > 0 ? 'BUY' : 'SELL';
      }
      const key = `${s.ticker}|${s.kind}|${s.time_str}|${s.price ?? ''}`;
      const prev = prevByKey.get(key);
      return {
        id: prev?.id ?? signalNow + Math.random(),
        type: s.kind,
        ticker: s.ticker,
        age: s.time_str,
        conf: s.confidence,
        side: mappedSide as any,
        text: s.kind,
        value: `${Math.round(Math.abs(s.price || 0))}`,
        count: prev?.count ?? 1,
        timestamp: prev?.timestamp ?? signalNow,
        price: s.price,
      };
    });

    // Coin switch: the backend re-points its cached book to the newly selected
    // ticker over the next tick. Until then `?? state.orderbook` would render
    // the PREVIOUS coin's levels on the new ladder (looks empty/sparse because
    // those prices fall outside the new grid). Drop the carried book on switch.
    // D1-FIX: Compare normalized tickers to avoid false positives from format mismatch
    const tickerChanged = !!data.selected_ticker && 
      normalizeTicker(data.selected_ticker) !== normalizeTicker(state.activeTicker);

    // PERF-02: Update orderbook and density in mutable buffers
    const currentTicker = data.selected_ticker || state.activeTicker;
    const buffer = marketDataService.getBuffer(currentTicker);
    
    // Update orderbook buffer if data present
    if (data.bids_top20 || data.asks_top20) {
      const bids = data.bids_top20 ?? (tickerChanged ? [] : state.orderbook.bids);
      const asks = data.asks_top20 ?? (tickerChanged ? [] : state.orderbook.asks);
      const mid = data.ob_mid ?? (tickerChanged ? 0 : state.orderbook.mid);
      const spreadBps = data.ob_spread_bps ?? (tickerChanged ? 0 : state.orderbook.spreadBps);
      const imbalance = data.ob_imbalance ?? (tickerChanged ? 0 : state.orderbook.imbalance);
      
      buffer.orderbook.update(bids, asks, mid, spreadBps, imbalance);
    }
    
    // Update density buffer if data present
    if (Array.isArray(data.density_history) && data.density_history.length > 0) {
      for (const col of data.density_history as DensityColumn[]) {
        buffer.density.push(col);
      }
    }
    
    // Clear buffers on ticker switch
    if (tickerChanged) {
      buffer.density.clear();
      buffer.chart.clear();
    }

    return {
      killSwitchActive: data.kill_switch_active ?? state.killSwitchActive,
      recorderActive: data.recorder_active ?? state.recorderActive,
      recorderPath: data.recorder_path ?? state.recorderPath,
      equity: acc.equity_usd ?? state.equity,
      unrealizedPnl: acc.unrealized_pnl_usd ?? state.unrealizedPnl,
      freeBalance: acc.free_balance_usd ?? state.freeBalance,
      sessionPnL: acc.realized_pnl_today_usd ?? state.sessionPnL,
      sessionPnLPct: acc.realized_pnl_today_usd && acc.starting_equity_usd
        ? (acc.realized_pnl_today_usd / acc.starting_equity_usd) * 100
        : state.sessionPnLPct,
      marginUsedPct: risk.margin_used_pct ?? state.marginUsedPct,
      dailyDrawdownPct: risk.current_drawdown_pct ?? state.dailyDrawdownPct,
      risk: {
        exposurePct: risk.exposure_pct ?? state.risk.exposurePct,
        totalTradesToday: risk.total_trades_today ?? state.risk.totalTradesToday,
        consecutiveLosses: risk.consecutive_losses ?? state.risk.consecutiveLosses
      },
      latency: data.metascalp?.latency_ms ?? state.latency,
      tickerPrices: newPrices,
      trades: data.open_trades ? trades : state.trades,
      lastStateGen: data.state_gen ?? state.lastStateGen,
      signals: newSignals.length ? newSignals : state.signals,
      strategyStates: data.strategy_states ?? state.strategyStates,
      orderbook: (function() {
        const bids = data.bids_top20 ?? (tickerChanged ? [] : state.orderbook.bids);
        const asks = data.asks_top20 ?? (tickerChanged ? [] : state.orderbook.asks);
        const mid = data.ob_mid ?? (tickerChanged ? 0 : state.orderbook.mid);
        const spreadBps = data.ob_spread_bps ?? (tickerChanged ? 0 : state.orderbook.spreadBps);
        const imbalance = data.ob_imbalance ?? (tickerChanged ? 0 : state.orderbook.imbalance);
        
        // D4-FIX: identity check to avoid unnecessary React re-renders
        if (!tickerChanged &&
            bids === state.orderbook.bids &&
            asks === state.orderbook.asks &&
            mid === state.orderbook.mid &&
            spreadBps === state.orderbook.spreadBps &&
            imbalance === state.orderbook.imbalance) {
          return state.orderbook;
        }
        return { bids, asks, mid, spreadBps, imbalance };
      })(),
      journalEntries,
      equityHistory: equityHistory.length ? equityHistory : state.equityHistory,
      spreadHistory: spreadHistory.length ? spreadHistory : state.spreadHistory,
      aggressionHistory: aggressionHistory.length ? aggressionHistory : state.aggressionHistory,
      volumeHistory: volumeHistory.length ? volumeHistory : state.volumeHistory,
      // D1-FIX: normalize selected_ticker so activeTicker key is always canonical
      activeTicker: data.selected_ticker ? normalizeTicker(data.selected_ticker) : state.activeTicker,
      chartHistory: mergedChartHistory,
      // D3-FIX: Clear density and iceberg on ticker switch to prevent stale visualization
      densityHistory: tickerChanged ? [] : (Array.isArray(data.density_history) ? (data.density_history as DensityColumn[]) : state.densityHistory),
      icebergEvents: tickerChanged ? [] : (data.iceberg_events ?? state.icebergEvents),
      universeTickers,
    };
  }),

}));
