import React, { useState, useMemo, useEffect, useRef } from 'react';
import {
  Layers,
  ArrowRightLeft, Target, Briefcase, Cpu, Terminal
} from 'lucide-react';
import { cn } from './lib/utils';
import { StrategyReadyState } from './types';
import { useTradeStore, Signal } from './store/useTradeStore';
import { useWsTransport } from './transport/useWsTransport';
import { apiToggleKillSwitch, apiSelectTicker, apiSendCommand, apiStartDump, apiStopDump } from './transport/api';
import { Ladder } from './components/Ladder';
import { AdvancedChart } from './components/AdvancedChart';
import { PhasePortrait } from './components/PhasePortrait';
import { WsStatusBadge } from './components/WsStatusBadge';
import { ErrorBoundary } from './components/ErrorBoundary';
import { CanvasMiniChart } from './components/CanvasMiniChart';
import { Journal } from './components/Journal';
import { CommandCenter } from './components/CommandCenter';
import { LogsView } from './components/LogsView';
import { StrategiesView } from './components/StrategiesView';
import { StatCard, ConditionBar } from './components/ui/shared';

function CommandInput() {
  const [cmd, setCmd] = React.useState('');
  const [result, setResult] = React.useState<string | null>(null);
  const [err, setErr] = React.useState<string | null>(null);

  const send = async () => {
    if (!cmd.trim()) return;
    setResult(null); setErr(null);
    try {
      await apiSendCommand(cmd.trim());
      setResult('OK');
      setCmd('');
    } catch (e: any) {
      setErr(e.message);
    }
  };

  return (
    <div className="border-t border-[#141e30] bg-[#0a0f1a] p-3 flex items-center gap-2 shrink-0">
      <input
        value={cmd}
        onChange={e => setCmd(e.target.value)}
        onKeyDown={e => e.key === 'Enter' && send()}
        placeholder="Enter command..."
        className="flex-1 bg-[#0d1422] border border-[#141e30] rounded px-3 py-1.5 text-[11px] font-mono text-[#b0c0d4] placeholder-[#3c4e62] focus:outline-none focus:border-[#c08828]/50"
      />
      <button
        onClick={send}
        className="px-4 py-1.5 bg-[#c08828]/15 text-[#c08828] hover:bg-[#c08828] hover:text-[#04060c] rounded text-[9px] font-black uppercase tracking-widest transition-colors border border-[#c08828]/30"
      >
        Send
      </button>
      {result && <span className="text-[10px] text-[#1a9448] font-mono">✓ {result}</span>}
      {err && <span className="text-[10px] text-[#ae2c2c] font-mono">✗ {err}</span>}
    </div>
  );
}


function SidebarItem({ icon: Icon, label, active, onClick, badge }: any) {
  return (
    <button
      onClick={onClick}
      title={label}
      className={cn(
        "w-[34px] h-[34px] flex items-center justify-center rounded relative transition-colors",
        active ? "text-[#c08828] bg-[#120d04]" : "text-[#3c4e62] hover:text-[#b0c0d4] hover:bg-[#0d1422]"
      )}
    >
      {active && <div className="absolute left-0 top-1/2 -translate-y-1/2 w-[2px] h-[18px] bg-[#c08828]" />}
      <Icon size={15} />
      {badge && (
        <span className="absolute -top-1 -right-1 w-2 h-2 bg-[#ae2c2c] rounded-full text-[0px]">{badge}</span>
      )}
    </button>
  );
}

function StatusDot({ state }: { state: StrategyReadyState }) {
  switch (state) {
    case StrategyReadyState.Ready: return <div className="w-2 h-2 bg-[#1a9448] rounded-full animate-pulse shadow-[0_0_8px_#1a9448]" title="Ready" />;
    case StrategyReadyState.Warming: return <div className="w-2 h-2 bg-[#c08828] rounded-full" title="Warming" />;
    case StrategyReadyState.Trading: return <div className="w-2 h-2 bg-[#3478b8] rounded-full animate-pulse shadow-[0_0_8px_#3478b8]" title="Trading" />;
    case StrategyReadyState.Planning: return <div className="w-2 h-2 bg-[#8b5cf6] rounded-full" title="Planning" />;
    case StrategyReadyState.Cooldown: return <div className="w-2 h-2 bg-[#1c2a38] rounded-full" title="Cooldown" />;
    default: return <div className="w-2 h-2 bg-[#1c2a38] rounded-full border border-[#141e30]" title="Cold" />;
  }
}

export default function App() {
  useWsTransport();

  const [activeTab, setActiveTab] = useState('market');
  const {
    killSwitchActive, toggleKillSwitch, equity, sessionPnL, sessionPnLPct,
    marginUsedPct, dailyDrawdownPct, trades, signals, strategyStates,
    activeTicker, setActiveTicker, equityHistory, spreadHistory,
    aggressionHistory, volumeHistory, unrealizedPnl, freeBalance,
    risk, orderbook, tickerPrices, universeTickers,
    journalEntries, wsStatus, recorderActive, recorderPath,
  } = useTradeStore();
  // PERF-02: chartHistory removed — AdvancedChart reads directly from MarketDataService

  const winRateLabel = useMemo(() => {
    if (!journalEntries.length) return 'N/A';
    const wins = journalEntries.filter(j => j.pnlUsd > 0).length;
    return `${Math.round((wins / journalEntries.length) * 100)}%`;
  }, [journalEntries]);

  // Realtime clock — drives "Xs ago" labels and the recent-signal dot so they
  // actually tick instead of freezing until an unrelated re-render.
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNowMs(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  // Honest connection state (replaces the hardcoded "PROD · LIVE" badge).
  const wsLabel = wsStatus === 'connected' ? 'PROD · LIVE'
    : wsStatus === 'connecting' ? 'CONNECTING…'
    : wsStatus === 'reconnecting' ? 'RECONNECTING…'
    : 'OFFLINE';
  const wsColor = wsStatus === 'connected' ? '#1a9448'
    : wsStatus === 'disconnected' ? '#ae2c2c' : '#c08828';

  // Two-stage kill-switch confirm — replaces the blocking window.confirm()
  // that froze the chart rAF loop and every live update while open.
  const [killArmed, setKillArmed] = useState(false);
  const killArmTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  useEffect(() => () => { if (killArmTimer.current) clearTimeout(killArmTimer.current); }, []);
  const handleKillClick = () => {
    if (!killArmed) {
      setKillArmed(true);
      if (killArmTimer.current) clearTimeout(killArmTimer.current);
      killArmTimer.current = setTimeout(() => setKillArmed(false), 4000);
      return;
    }
    if (killArmTimer.current) clearTimeout(killArmTimer.current);
    setKillArmed(false);
    toggleKillSwitch();
    apiToggleKillSwitch().catch(console.error);
  };

  // Recording state — source of truth is store.recorderActive (synced from backend).
  // recordingTickers is captured at start so we can show the perf_replay command.
  const [recordingTickers, setRecordingTickers] = useState<string[]>([]);
  const [replayCmd, setReplayCmd] = useState('');

  const handleRecordClick = () => {
    if (recorderActive) {
      apiStopDump().catch(console.error);
      const tickerFlags = recordingTickers.map(t => `--ticker ${t}`).join(' ');
      setReplayCmd(`perf_replay --dump ${recorderPath} ${tickerFlags}`);
    } else {
      const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
      setRecordingTickers([...universeTickers]);
      setReplayCmd('');
      apiStartDump(`dump_${ts}`).catch(console.error);
    }
  };

  const [sigFilter, setSigFilter] = useState<'ALL' | 'BUY' | 'SELL'>('ALL');
  const [sigPaused, setSigPaused] = useState(false);
  const [pausedSignals, setPausedSignals] = useState<Signal[]>([]);

  const numOpenTrades = useMemo(() => trades.filter(t => t.status === 'OPEN').length, [trades]);

  // Deduplicate strategy tickers, then append universe-only tickers
  const tabTickers = useMemo(() => {
    const seen = new Set<string>();
    const result: string[] = [];
    for (const s of strategyStates) {
      if (!seen.has(s.ticker)) { seen.add(s.ticker); result.push(s.ticker); }
    }
    for (const t of universeTickers) {
      if (!seen.has(t)) { seen.add(t); result.push(t); }
    }
    return result.slice(0, 12);
  }, [strategyStates, universeTickers]);

  const spreadArr = useMemo(() => spreadHistory.map(p => p.val), [spreadHistory]);
  const aggressionArr = useMemo(() => aggressionHistory.map(p => p.val), [aggressionHistory]);
  const volArr = useMemo(() => volumeHistory.map(p => p.buy + p.sell), [volumeHistory]);
  const sparkData = useMemo(() => equityHistory.slice(-15).map(p => ({ v: p.val })), [equityHistory]);

  const displaySignals = sigPaused ? pausedSignals : signals;
  const filteredSignals = displaySignals.filter(s =>
    sigFilter === 'ALL' || s.side === sigFilter ||
    (sigFilter === 'BUY' && s.side === 'LONG') ||
    (sigFilter === 'SELL' && s.side === 'SHORT')
  ).slice(0, 30);

  const currentPrice = tickerPrices[activeTicker] ?? 0;
  const lastAggression = aggressionHistory.at(-1)?.val ?? null;

  return (
    <div className={cn("flex h-screen bg-[#04060c] text-[#b0c0d4] font-sans overflow-hidden", killSwitchActive && "ring-[3px] ring-inset ring-[#ae2c2c]")}>

      {/* Sidebar */}
      <aside className="w-[42px] bg-[#070b13] border-r border-[#1a2535] flex flex-col items-center py-1.5 gap-[1px] shrink-0 z-50">
        <SidebarItem icon={ArrowRightLeft} label="Execution" active={activeTab === 'market'} onClick={() => setActiveTab('market')} />
        <SidebarItem icon={Target} label="Command" active={activeTab === 'command'} onClick={() => setActiveTab('command')} />
        <SidebarItem icon={Briefcase} label="Portfolio" active={activeTab === 'portfolio'} onClick={() => setActiveTab('portfolio')} badge={numOpenTrades > 0} />
        <SidebarItem icon={Layers} label="Journal" active={activeTab === 'journal'} onClick={() => setActiveTab('journal')} />
        <div className="w-5 h-px bg-[#141e30] my-1" />
        <SidebarItem icon={Cpu} label="Strategies" active={activeTab === 'strategies'} onClick={() => setActiveTab('strategies')} />
        <SidebarItem icon={Terminal} label="Logs" active={activeTab === 'logs'} onClick={() => setActiveTab('logs')} />
      </aside>

      <main className="flex-1 flex flex-col min-w-0 overflow-hidden">

        {/* Topbar */}
        <header className="h-10 bg-[#070b13] border-b border-[#1a2535] flex items-center shrink-0">
          {/* Brand */}
          <div className="w-[138px] shrink-0 flex items-center gap-2 px-3 border-r border-[#1a2535]">
            <div className="w-5 h-5 bg-[#c08828] flex items-center justify-center font-mono text-[9px] font-bold text-[#04060c]">TB</div>
            <div>
              <div className="text-[10px] font-semibold tracking-[.15em] text-[#d8e8f8] leading-tight">TRADEBOT</div>
              <div className="font-mono text-[8px] tracking-[.1em]" style={{ color: wsColor }}>{wsLabel}</div>
            </div>
          </div>

          {/* Stats row — 4 semantic groups */}
          <div className="flex items-stretch">
            {/* Group: Account */}
            <div className="flex items-stretch border-r border-[#1a2535]">
              <div className="flex flex-col justify-center px-3 shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Equity</span>
                <span className="font-mono text-[13px] font-bold text-[#d8e8f8] leading-none">${equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</span>
              </div>
            </div>
            {/* Group: P&L */}
            <div className="flex items-stretch border-r border-[#1a2535]">
              <div className="flex flex-col justify-center px-3 border-r border-[#141e30] shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Session PnL</span>
                <span className={cn("font-mono text-[13px] font-bold leading-none", sessionPnL >= 0 ? "text-[#1a9448]" : "text-[#ae2c2c]")}>
                  {sessionPnL >= 0 ? '+' : ''}{sessionPnL.toFixed(2)} · {sessionPnLPct.toFixed(2)}%
                </span>
              </div>
              <div className="flex flex-col justify-center px-3 shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Open PnL</span>
                <span className={cn("font-mono text-[13px] font-bold leading-none", unrealizedPnl >= 0 ? "text-[#1a9448]" : "text-[#ae2c2c]")}>
                  {unrealizedPnl >= 0 ? '+' : ''}{unrealizedPnl.toFixed(2)}
                </span>
              </div>
            </div>
            {/* Group: Risk */}
            <div className="flex items-stretch border-r border-[#1a2535]">
              <div className="flex flex-col justify-center px-3 border-r border-[#141e30] shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Margin used</span>
                <span className="font-mono text-[13px] font-bold text-[#d8e8f8] leading-none">{marginUsedPct.toFixed(1)}%</span>
              </div>
              <div className="flex flex-col justify-center px-3 shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Drawdown</span>
                <span className="font-mono text-[13px] font-bold text-[#ae2c2c] leading-none">−{dailyDrawdownPct.toFixed(2)}%</span>
              </div>
            </div>
            {/* Group: Market */}
            <div className="flex items-stretch">
              <div className="flex flex-col justify-center px-3 border-r border-[#141e30] shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Spread</span>
                <span className="font-mono text-[13px] font-bold text-[#b0c0d4] leading-none">{Number.isFinite(orderbook.spreadBps) && orderbook.spreadBps > 0 ? orderbook.spreadBps.toFixed(1) + ' bps' : '—'}</span>
              </div>
              <div className="flex flex-col justify-center px-3 shrink-0 gap-0">
                <span className="text-[8px] opacity-50 font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Trades · Win</span>
                <span className="font-mono text-[13px] font-bold text-[#d8e8f8] leading-none">{risk.totalTradesToday} · {winRateLabel}</span>
              </div>
            </div>
          </div>

          {/* MARK price */}
          {currentPrice > 0 && (
            <div title="Current mark price from exchange" className="flex flex-col justify-center items-end px-4 border-l border-[#141e30] shrink-0 gap-0">
              <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Mark</span>
              <span className="font-mono text-[14px] font-bold text-[#c08828] leading-none">{currentPrice.toFixed(2)}</span>
            </div>
          )}

          {/* Right controls */}
          <div className="flex items-center gap-3 px-3 ml-auto shrink-0">
            {/* REC button + ticker badges */}
            <div className="flex items-center gap-1.5">
              {replayCmd && !recorderActive && (
                <div className="flex items-center gap-1 bg-[#0d1422] border border-[#1c2a38] px-2 py-1 rounded max-w-[260px]">
                  <span className="font-mono text-[7.5px] text-[#3c4e62] truncate" title={replayCmd}>{replayCmd}</span>
                  <button
                    onClick={() => { navigator.clipboard.writeText(replayCmd); }}
                    className="text-[#3c4e62] hover:text-[#b0c0d4] shrink-0 ml-1"
                    title="Copy perf_replay command"
                  >⧉</button>
                  <button onClick={() => setReplayCmd('')} className="text-[#3c4e62] hover:text-[#ae2c2c] shrink-0" title="Dismiss">✕</button>
                </div>
              )}
              {recorderActive && recordingTickers.length > 0 && (
                <div className="flex items-center gap-1">
                  {recordingTickers.slice(0, 3).map(t => (
                    <span key={t} className="font-mono text-[7px] text-[#ae2c2c]/70 bg-[#ae2c2c]/10 border border-[#ae2c2c]/20 px-1.5 py-0.5 rounded">
                      {(t.split(':')[1] ?? t).replace('.p','').replace('.m','')}
                    </span>
                  ))}
                  {recordingTickers.length > 3 && (
                    <span className="font-mono text-[7px] text-[#ae2c2c]/50">+{recordingTickers.length - 3}</span>
                  )}
                </div>
              )}
              <button
                onClick={handleRecordClick}
                title={recorderActive
                  ? `Stop recording → ${recorderPath}\n${recordingTickers.map(t => '--ticker ' + t).join(' ')}`
                  : `Record all ${universeTickers.length} active tickers to NDJSON (for perf_replay)`}
                className={cn(
                  "font-mono text-[8px] font-bold uppercase tracking-[.14em] px-3 py-1.5 border transition-colors flex items-center gap-1.5 shrink-0",
                  recorderActive
                    ? "bg-[#ae2c2c]/15 border-[#ae2c2c]/60 text-[#ae2c2c] hover:bg-[#ae2c2c]/25"
                    : "bg-[#0d1422] border-[#1c2a38] text-[#3c4e62] hover:text-[#b0c0d4] hover:border-[#3c4e62]"
                )}
              >
                <span className={cn("w-1.5 h-1.5 rounded-full shrink-0", recorderActive ? "bg-[#ae2c2c] animate-pulse" : "bg-[#3c4e62]")} />
                {recorderActive ? 'STOP REC' : 'REC'}
              </button>
            </div>
            <WsStatusBadge />
            <button
              onClick={handleKillClick}
              title={killArmed ? 'Click again to confirm' : killSwitchActive ? 'Recover from kill switch' : 'Activate kill switch'}
              className={cn(
                "font-mono text-[8px] font-bold uppercase tracking-[.14em] px-3 py-1.5 border transition-colors min-w-[120px]",
                killArmed
                  ? "bg-[#c08828] border-[#c08828] text-[#04060c] animate-pulse"
                  : killSwitchActive
                    ? "bg-[#ae2c2c] border-[#ae2c2c] text-white hover:bg-[#ae2c2c]/80"
                    : "bg-[#200808] border-[#ae2c2c] text-[#ae2c2c] hover:bg-[#ae2c2c] hover:text-white"
              )}>
              {killArmed
                ? (killSwitchActive ? 'CONFIRM RECOVER' : 'CONFIRM HALT')
                : killSwitchActive ? 'RECOVER' : '■ KILL SWITCH'}
            </button>
          </div>
        </header>

        {/* Tab Content */}
        <div className="flex-1 overflow-y-auto w-full relative">

          {activeTab === 'market' && (
            <div className="absolute inset-0 flex overflow-hidden">
              {/* LADDER */}
              <div className="w-[198px] shrink-0 border-r border-[#141e30] h-full overflow-hidden bg-[#070b13]">
                <Ladder ticker={activeTicker} />
              </div>

              {/* CENTER: Chart */}
              <div className="flex-1 min-w-0 border-r border-[#141e30] h-full flex flex-col">
                {/* Chart tabs */}
                <div className="h-7 bg-[#0a0f1a] border-b border-[#141e30] flex items-stretch shrink-0">
                  <div className="flex items-stretch overflow-x-auto no-scrollbar">
                    {tabTickers.map((ticker, idx) => {
                      const short = (ticker.split(':')[1] ?? ticker).replace('.p', '').replace('.m', '');
                      const price = tickerPrices[ticker];
                      const hasStrategy = strategyStates.some(s => s.ticker === ticker);
                      const hasRecentSignal = signals.some(sig => {
                        const st = (sig.ticker.split(':')[1] ?? sig.ticker).replace('.p','').replace('.m','');
                        return st === short && (nowMs - sig.timestamp) < 30000;
                      });
                      return (
                        <button
                          key={`tab-${idx}-${ticker}`}
                          onClick={() => { setActiveTicker(ticker); apiSelectTicker(ticker).catch(console.error); }}
                          className={cn(
                            "relative font-mono text-[9px] px-3 border-r border-[#141e30] transition-colors flex items-center gap-1 shrink-0",
                            activeTicker === ticker
                              ? "text-[#c08828] bg-[#120d04] border-b border-[#c08828]"
                              : "text-[#3c4e62] hover:text-[#b0c0d4] hover:bg-[#0d1422]"
                          )}
                        >
                          {!hasStrategy && <span className="w-1 h-1 rounded-full bg-[#2a3a4e] shrink-0" title="No active strategy" />}
                          <span className="truncate max-w-[84px]" title={short}>{short}</span>
                          {price ? <span className="font-mono text-[8px] text-[#3c4e62]">{price > 100 ? price.toFixed(0) : price < 1 ? price.toFixed(4) : price.toFixed(2)}</span> : null}
                          {hasRecentSignal && <span className="absolute top-0.5 right-0.5 w-1.5 h-1.5 bg-[#c08828] rounded-full" />}
                        </button>
                      );
                    })}
                  </div>
                  <div className="flex items-center px-1 text-[#3c4e62] text-[8px] shrink-0">›</div>
                  {/* Honest mode indicator — the chart is tick-streamed, not
                      candle/timeframe based; a fake TF switcher was removed. */}
                  <div className="ml-auto flex items-center border-l border-[#141e30] px-2.5 shrink-0">
                    <span className="font-mono text-[8px] text-[#3c4e62] tracking-[.1em]" title="Tick-by-tick live stream (wheel to zoom the chart time window)">
                      TICK · LIVE
                    </span>
                  </div>
                </div>

                {/* Chart body */}
                <div className="flex-1 relative min-h-0 bg-[#04060c]">
                  <ErrorBoundary fallbackLabel="Chart">
                    <AdvancedChart ticker={activeTicker} />
                  </ErrorBoundary>

                  {/* Floating open positions */}
                  <div className="absolute top-3 left-3 z-10 flex flex-col gap-1.5 pointer-events-none">
                    {trades.filter(t => t.ticker === activeTicker || t.ticker === activeTicker.split(':')[1]).map(t => (
                      <div key={t.id} className="bg-[rgba(7,11,19,0.92)] backdrop-blur-sm border border-[#141e30] border-l-2 border-l-[#1a9448] px-2.5 py-2 min-w-[148px]">
                        <div className="text-[8px] font-bold uppercase tracking-[.14em] text-[#1a9448] mb-0.5">{t.side}</div>
                        <div className="text-[8px] text-[#3c4e62] mb-0.5">{t.strategy}</div>
                        <div className={cn("font-mono text-[15px] font-bold", (t.pnl_usd || 0) >= 0 ? "text-[#1a9448]" : "text-[#ae2c2c]")}>
                          {(t.pnl_usd || 0) >= 0 ? '+' : ''}{(t.pnl_usd || 0).toFixed(2)}
                        </div>
                        <div className="font-mono text-[8px] text-[#3c4e62] mt-0.5">Entry {t.entry_price}</div>
                      </div>
                    ))}
                  </div>
                </div>
              </div>

              {/* RIGHT: Analytics + Signals */}
              <div className="w-[212px] shrink-0 bg-[#070b13] h-full flex flex-col overflow-y-auto no-scrollbar">
                <div className="h-7 bg-[#0a0f1a] border-b border-[#141e30] flex items-center px-2.5 shrink-0">
                  <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.12em]">Analytics</span>
                </div>

                <div className="flex flex-col shrink-0">
                  <PhasePortrait />
                  {lastAggression !== null && (
                    <div className="px-2 py-1 border-b border-[#0c1220] grid grid-cols-3 gap-0">
                      <div className="text-center" title="Tape Aggression — buy vs sell pressure ratio (-1 to +1)">
                        <div className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Aggr</div>
                        <div className={cn("font-mono text-[10px] font-bold", lastAggression > 0 ? "text-[#1a9448]" : "text-[#ae2c2c]")}>
                          {lastAggression.toFixed(2)}
                        </div>
                      </div>
                      <div className="text-center" title="Order Book Imbalance — bid/ask volume ratio">
                        <div className="text-[8px] text-[#3c4e62] uppercase">Imbal</div>
                        <div className="font-mono text-[10px] font-bold text-[#3478b8]">{orderbook.imbalance.toFixed(2)}</div>
                      </div>
                      <div className="text-center" title="Prints per second — execution frequency">
                        <div className="text-[8px] text-[#3c4e62] uppercase">Prnt/s</div>
                        <div className="font-mono text-[10px] text-[#b0c0d4]">—</div>
                      </div>
                    </div>
                  )}
                  <div className="px-2.5 py-1 border-b border-[#0c1220]">
                    <div className="flex justify-between items-center mb-1">
                      <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Spread</span>
                      <span className={cn("font-mono text-[10px] font-semibold", orderbook.spreadBps < 3 ? "text-[#1a9448]" : orderbook.spreadBps > 15 ? "text-[#ae2c2c]" : "text-[#c08828]")}>{orderbook.spreadBps > 0 ? orderbook.spreadBps.toFixed(1) + ' bps' : '—'}</span>
                    </div>
                    <div className="h-5">
                      <CanvasMiniChart data={spreadArr} color="#c08828" />
                    </div>
                  </div>
                  <div className="px-2.5 py-1 border-b border-[#0c1220]">
                    <div className="flex justify-between items-center mb-1">
                      <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Tape aggression</span>
                      <span className="font-mono text-[10px] font-semibold text-[#3478b8]">
                        {aggressionArr[aggressionArr.length - 1]?.toFixed(2) ?? '—'}
                      </span>
                    </div>
                    <div className="h-5">
                      <CanvasMiniChart data={aggressionArr} color="#3478b8" />
                    </div>
                  </div>
                  <div className="px-2.5 py-1 border-b border-[#0c1220]">
                    <div className="flex justify-between items-center mb-1">
                      <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Volume 1m</span>
                      <span className="font-mono text-[10px] font-semibold text-[#d8e8f8]">
                        {volArr[volArr.length - 1]?.toFixed(2) ?? '—'}
                      </span>
                    </div>
                    <div className="h-5">
                      <CanvasMiniChart data={volArr} color="#8b5cf6" />
                    </div>
                  </div>
                </div>

                {/* Signals */}
                <div className="h-7 bg-[#0a0f1a] border-y border-[#141e30] flex items-center justify-between px-2.5 shrink-0">
                  <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.12em]">Signals</span>
                  <div className="flex items-center gap-1.5">
                    {!sigPaused && <span className="w-1 h-1 bg-[#1a9448] rounded-full animate-pulse" />}
                    <button
                      onClick={() => {
                        if (!sigPaused) setPausedSignals(signals);
                        setSigPaused(p => !p);
                      }}
                      className={cn("font-mono text-[8px] px-1.5 py-0.5 rounded border transition-colors",
                        sigPaused
                          ? "text-[#c08828] border-[#c08828]/40 bg-[#c08828]/10 hover:bg-[#c08828]/20"
                          : "text-[#3c4e62] border-[#1c2a38] hover:text-[#b0c0d4]"
                      )}
                    >
                      {sigPaused ? '▶ RESUME' : '⏸ PAUSE'}
                    </button>
                  </div>
                </div>
                <div className="flex items-center gap-1 px-2 py-1 border-b border-[#0c1220] bg-[#070b13] shrink-0">
                  {(['ALL', 'BUY', 'SELL'] as const).map(f => (
                    <button key={f} onClick={() => setSigFilter(f)}
                      className={cn("font-mono text-[8px] px-2 py-0.5 rounded border transition-colors",
                        sigFilter === f
                          ? f === 'BUY' ? "text-[#1a9448] border-[#1a9448]/40 bg-[#1a9448]/10"
                            : f === 'SELL' ? "text-[#ae2c2c] border-[#ae2c2c]/40 bg-[#ae2c2c]/10"
                            : "text-[#c08828] border-[#c08828]/40 bg-[#c08828]/10"
                          : "text-[#3c4e62] border-[#1c2a38] hover:text-[#b0c0d4]"
                      )}
                    >{f}</button>
                  ))}
                </div>
                <div className="flex-1 overflow-y-auto styled-scrollbar">
                  {filteredSignals.map((s, idx) => {
                    const isBuy = s.side === 'BUY' || s.side === 'LONG';
                    const isSell = s.side === 'SELL' || s.side === 'SHORT';
                    const conf100 = s.conf >= 0.99;
                    const confHigh = s.conf >= 0.8;
                    const shortTicker = (s.ticker.split(':')[1] ?? s.ticker).replace('.p', '').replace('.m', '');
                    const secsAgo = Math.max(0, Math.round((nowMs - s.timestamp) / 1000));
                    const timeLabel = secsAgo < 60 ? `${secsAgo}s` : `${Math.floor(secsAgo/60)}m`;
                    const priceStr = s.price ? `@${s.price < 1 ? s.price.toFixed(4) : s.price.toFixed(2)}` : '';
                    return (
                      <div
                        key={`sig-${idx}-${s.id}-${s.timestamp}`}
                        onClick={() => { setActiveTicker(s.ticker); apiSelectTicker(s.ticker).catch(console.error); }}
                        className={cn(
                          "px-2.5 py-[5px] border-b border-[#0c1220] cursor-pointer hover:bg-[#0d1422] transition-colors",
                          isBuy ? "border-l-2 border-l-[#1a9448]" : isSell ? "border-l-2 border-l-[#ae2c2c]" : "border-l-2 border-l-transparent"
                        )}
                      >
                        <div className="flex items-center gap-1.5 mb-0.5">
                          <span className={cn(
                            "text-[8px] font-black uppercase tracking-wider px-1 py-px rounded",
                            isBuy
                              ? conf100 ? "bg-[#26c26e]/20 text-[#26c26e]" : "bg-[#1a9448]/15 text-[#1a9448]"
                              : isSell
                                ? conf100 ? "bg-[#e05050]/20 text-[#e05050]" : "bg-[#ae2c2c]/15 text-[#ae2c2c]"
                                : "bg-[#c08828]/15 text-[#c08828]"
                          )}>{s.side}</span>
                          <span className="font-mono text-[8px] font-bold text-[#b0c0d4]">{shortTicker}</span>
                          <span className="font-mono text-[8px] text-[#3c4e62] ml-auto">{timeLabel}</span>
                        </div>
                        <div className="flex items-center justify-between">
                          <span className="text-[9px] font-semibold text-[#b0c0d4] leading-tight">{s.type}</span>
                          <div className="flex items-center gap-1.5">
                            {priceStr && <span className="font-mono text-[7.5px] text-[#3c4e62]">{priceStr}</span>}
                            <span className={cn("font-mono text-[9px] font-bold",
                              conf100 ? (isBuy ? "text-[#26c26e]" : "text-[#e05050]") : confHigh ? "text-[#1a9448]" : "text-[#3c4e62]"
                            )}>{Math.round(s.conf * 100)}%</span>
                          </div>
                        </div>
                        <div className="mt-0.5 h-[2px] bg-[#0c1220] rounded-full overflow-hidden">
                          <div className={cn("h-full", isBuy ? "bg-[#1a9448]" : "bg-[#ae2c2c]")}
                               style={{ width: `${Math.round(s.conf * 100)}%` }} />
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            </div>
          )}

          {activeTab === 'command' && (
            <div className="h-full flex flex-col">
              <div className="flex-1 overflow-y-auto min-h-0">
                <CommandCenter equityHistory={equityHistory} sparkData={sparkData} />
              </div>
              <CommandInput />
            </div>
          )}

          {/* PORTFOLIO & RISK */}
          {activeTab === 'portfolio' && (
            <div className="max-w-[1400px] mx-auto space-y-6 p-8">
              <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
                {trades.map(t => (
                  <div key={t.id} className={cn("glass-card p-6 relative overflow-hidden group transition-all", killSwitchActive && "animate-pulse border-[#ae2c2c] bg-[#ae2c2c]/10")}>
                    <div className="flex items-center justify-between mb-4">
                      <div className="flex items-center gap-4">
                        <div className={cn("px-3 py-1.5 rounded text-[11px] font-black uppercase text-center min-w-[70px]", t.side === 'LONG' ? "bg-[#1a9448]/15 text-[#1a9448]" : "bg-[#ae2c2c]/15 text-[#ae2c2c]")}>
                          {t.side}
                        </div>
                        <div>
                          <div className="text-xl font-bold text-[#d8e8f8] max-w-[150px] truncate">{t.ticker}</div>
                          <div className="text-[10px] text-[#3c4e62] font-bold">{t.strategy}</div>
                        </div>
                      </div>
                      <div className="text-right">
                        <div className="text-[10px] font-bold text-[#3c4e62] uppercase mb-1">Unrealized PnL</div>
                        <div className={cn("text-2xl font-mono font-black", (t.pnl_usd || 0) >= 0 ? "text-[#1a9448]" : "text-[#ae2c2c]")}>
                          {(t.pnl_usd || 0) >= 0 ? '+' : ''}{(t.pnl_usd || 0).toFixed(2)}
                        </div>
                      </div>
                    </div>

                    {(() => {
                      // Real SL/TP from the trade (was hardcoded -1.5% / +3.0%).
                      const entry = t.entry_price;
                      const isLong = t.side === 'LONG';
                      const dir = isLong ? 1 : -1;
                      const slPct = t.stopLoss && entry ? ((t.stopLoss - entry) / entry) * 100 * dir : null;
                      const tpPct = t.takeProfit && entry ? ((t.takeProfit - entry) / entry) * 100 * dir : null;
                      // Progress of mark price from SL (0%) to TP (100%).
                      let prog = 0;
                      if (t.stopLoss && t.takeProfit) {
                        const span = Math.abs(t.takeProfit - t.stopLoss) || 1;
                        const trav = isLong ? t.mark_price - t.stopLoss : t.stopLoss - t.mark_price;
                        prog = Math.max(0, Math.min(100, (trav / span) * 100));
                      }
                      const hasLevels = !!(t.stopLoss && t.takeProfit);
                      return (
                        <div className="mt-8 relative">
                          <div className="flex justify-between text-[9px] font-bold text-[#3c4e62] uppercase mb-2">
                            <span>SL {t.stopLoss ? `${t.stopLoss} (${slPct! >= 0 ? '+' : ''}${slPct!.toFixed(2)}%)` : '—'}</span>
                            <span>TP {t.takeProfit ? `${t.takeProfit} (${tpPct! >= 0 ? '+' : ''}${tpPct!.toFixed(2)}%)` : '—'}</span>
                          </div>
                          <div className="w-full h-1.5 bg-black/40 rounded-full overflow-hidden border border-[#141e30] relative">
                            {hasLevels ? (
                              <div className={cn("absolute left-0 top-0 bottom-0", isLong ? "bg-[#1a9448]" : "bg-[#ae2c2c]")} style={{ width: `${prog}%` }} />
                            ) : (
                              <div className="absolute inset-0 flex items-center justify-center text-[8px] font-mono text-[#3c4e62] uppercase tracking-wider">No SL/TP data</div>
                            )}
                          </div>
                        </div>
                      );
                    })()}

                    <div className="mt-6 flex justify-end">
                      <button onClick={() => apiSendCommand('close ' + t.ticker).catch(console.error)} className="px-6 py-2 bg-[#ae2c2c]/15 text-[#ae2c2c] hover:bg-[#ae2c2c] hover:text-white rounded text-[10px] font-black uppercase tracking-widest transition-all">
                        Market Close
                      </button>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          )}

          {/* LOGS TAB */}
          {activeTab === 'logs' && (
            <div className="absolute inset-0">
              <LogsView />
            </div>
          )}

          {/* JOURNAL TAB — Journal renders its own header bar (no duplicate title) */}
          {activeTab === 'journal' && (
            <div className="absolute inset-0">
              <Journal />
            </div>
          )}

          {/* STRATEGIES TAB */}
          {activeTab === 'strategies' && (
            <div className="absolute inset-0 overflow-y-auto">
              <StrategiesView />
            </div>
          )}

        </div>
      </main>
    </div>
  );
}
