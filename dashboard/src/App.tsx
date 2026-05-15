import React, { useState, useMemo } from 'react';
import {
  TrendingUp, Activity, Zap, Server, Layers,
  ArrowRightLeft, Target, Fingerprint, Crosshair, BarChart, Briefcase, PieChart,
  Volume2, VolumeX, ShieldAlert, AlertTriangle, Terminal, Cpu
} from 'lucide-react';
import {
  ResponsiveContainer, AreaChart, Area, BarChart as ReBarChart, Bar,
  Radar, RadarChart, PolarGrid, PolarAngleAxis, PieChart as RePieChart, Pie, Cell, Tooltip
} from 'recharts';
import { cn } from './lib/utils';
import { StrategyReadyState } from './types';
import { useTradeStore } from './store/useTradeStore';
import { useWsTransport } from './transport/useWsTransport';
import { apiToggleKillSwitch, apiSelectTicker, apiSendCommand, apiStartDump, apiStopDump } from './transport/api';
import { Ladder } from './components/Ladder';
import { CandlestickChart } from './components/Chart';
import { CanvasMiniChart } from './components/CanvasMiniChart';
import { Journal } from './components/Journal';
import { CommandCenter } from './components/CommandCenter';
import { LogsView } from './components/LogsView';
import { TradingPanel } from './components/TradingPanel';
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
    <div className="border-t border-white/5 bg-black/40 p-4 flex items-center gap-3 shrink-0">
      <input
        value={cmd}
        onChange={e => setCmd(e.target.value)}
        onKeyDown={e => e.key === 'Enter' && send()}
        placeholder="Enter command..."
        className="flex-1 bg-white/[0.04] border border-white/10 rounded-lg px-4 py-2 text-xs font-mono text-white placeholder-[#7b859c] focus:outline-none focus:border-[#58a6ff]/50"
      />
      <button
        onClick={send}
        className="px-5 py-2 bg-[#58a6ff]/15 text-[#58a6ff] hover:bg-[#58a6ff] hover:text-white rounded-lg text-[10px] font-black uppercase tracking-widest transition-colors border border-[#58a6ff]/30"
      >
        Send
      </button>
      {result && <span className="text-[10px] text-[#10b981] font-mono">✓ {result}</span>}
      {err && <span className="text-[10px] text-[#f43f5e] font-mono">✗ {err}</span>}
    </div>
  );
}


function SidebarItem({ icon: Icon, label, active, onClick, badge, alert }: any) {
  return (
    <button 
      onClick={onClick}
      className={cn(
        "w-full flex items-center justify-between px-4 py-3.5 rounded-xl transition-all duration-300 group relative overflow-hidden",
        active ? "bg-white/[0.08] text-white border border-white/10" : "text-[#7b859c] hover:bg-white/[0.04] hover:text-[#c0c7d4] border border-transparent"
      )}
    >
      {active && <div className="absolute left-0 top-1/2 -translate-y-1/2 w-1 h-8 bg-[#58a6ff] rounded-r shadow-[0_0_12px_#58a6ff]" />}
      <div className="flex items-center gap-3">
        <Icon size={18} className={cn("transition-transform duration-300", active ? "text-[#58a6ff] scale-110" : "group-hover:scale-110", alert && "text-rose-400")} />
        <span className={cn("text-xs font-semibold tracking-tight", active ? "opacity-100" : "opacity-90")}>{label}</span>
      </div>
      {badge && (
        <span className={cn("text-[9px] font-black px-2 py-0.5 rounded flex items-center justify-center", 
          alert ? "bg-rose-500/20 text-rose-400 border border-rose-500/30" : "bg-[#58a6ff]/20 text-[#58a6ff] border border-[#58a6ff]/30")}>
          {badge}
        </span>
      )}
    </button>
  );
}

function StatusDot({ state }: { state: StrategyReadyState }) {
  switch (state) {
    case StrategyReadyState.Ready: return <div className="w-2.5 h-2.5 bg-[#10b981] rounded-full animate-pulse shadow-[0_0_8px_#10b981]" title="Ready" />;
    case StrategyReadyState.Warming: return <div className="w-2 h-2 bg-[#f59e0b] rounded-full" title="Warming" />;
    case StrategyReadyState.Trading: return <div className="w-2 h-2 bg-[#58a6ff] rounded-full animate-pulse shadow-[0_0_8px_#58a6ff]" title="Trading" />;
    case StrategyReadyState.Planning: return <div className="w-2 h-2 bg-[#8b5cf6] rounded-full" title="Planning" />;
    case StrategyReadyState.Cooldown: return <div className="w-2 h-2 bg-[#30363d] rounded-full" title="Cooldown" />;
    default: return <div className="w-2 h-2 bg-[#1c2333] rounded-full border border-white/10" title="Cold" />;
  }
}

export default function App() {
  useWsTransport();

  const [activeTab, setActiveTab] = useState('market');
  const [audioEnabled, setAudioEnabled] = useState(true);

  // Zustand State
  const { killSwitchActive, toggleKillSwitch, latency, equity, sessionPnL, sessionPnLPct, marginUsedPct, dailyDrawdownPct, tickerPrices, trades, signals, strategyStates, activeTicker, setActiveTicker, equityHistory, wsConnected, spreadHistory, aggressionHistory, volumeHistory, recorderActive, unrealizedPnl, freeBalance } = useTradeStore();

  const spreadArr = useMemo(() => spreadHistory.map(p => p.val), [spreadHistory]);
  const aggressionArr = useMemo(() => aggressionHistory.map(p => p.val), [aggressionHistory]);
  const volArr = useMemo(() => volumeHistory.map(p => p.buy + p.sell), [volumeHistory]);
  const sparkData = useMemo(() => equityHistory.slice(-15).map(p => ({ v: p.val })), [equityHistory]);

  return (
    <div className={cn("flex h-screen bg-[#03050a] text-[#e8edf5] font-sans overflow-hidden transition-colors duration-500", killSwitchActive && "ring-[4px] ring-inset ring-[#f43f5e]")}>
      
      <div className="fixed inset-0 grid-bg opacity-30 pointer-events-none" />
      <div className="fixed top-[-20%] left-[-10%] w-[50%] h-[50%] bg-[#58a6ff]/5 rounded-full blur-[120px] pointer-events-none" />

      {/* Sidebar */}
      <aside className="w-[240px] glass-panel z-50 flex flex-col border-r border-[#ffffff0a]">
        <div className="p-6 pb-2">
          <div className="flex items-center gap-4 mb-2">
            <div className="w-9 h-9 bg-gradient-to-br from-[#58a6ff] to-[#3b82f6] rounded-xl flex items-center justify-center shadow-[0_0_15px_rgba(88,166,255,0.4)]">
              <Zap size={18} className="text-white fill-white" />
            </div>
            <div>
              <div className="text-sm font-black tracking-tight leading-none text-white">TRADEBOT PRO</div>
              <div className="text-[9px] font-bold text-[#58a6ff] uppercase tracking-[0.2em] mt-1.5">Prod Node</div>
            </div>
          </div>
        </div>

        <nav className="flex-1 px-4 py-6 space-y-1.5 overflow-y-auto no-scrollbar">
          <SidebarItem icon={ArrowRightLeft} label="Execution View" active={activeTab === 'market'} onClick={() => setActiveTab('market')} />
          <SidebarItem icon={Target} label="Command Center" active={activeTab === 'command'} onClick={() => setActiveTab('command')} />
          <SidebarItem icon={Briefcase} label="Portfolio" active={activeTab === 'portfolio'} onClick={() => setActiveTab('portfolio')} badge="2 Open" />
          <SidebarItem icon={Layers} label="Journal" active={activeTab === 'journal'} onClick={() => setActiveTab('journal')} />
          <SidebarItem icon={Cpu} label="Strategies" active={activeTab === 'strategies'} onClick={() => setActiveTab('strategies')} />
          <SidebarItem icon={Terminal} label="Logs" active={activeTab === 'logs'} onClick={() => setActiveTab('logs')} />
        </nav>

        <div className="p-4 border-t border-white/5 bg-black/20">
          <div className="flex items-center justify-between mb-3 px-2">
            <div className="flex items-center gap-2">
              <div className={cn("w-1.5 h-1.5 rounded-full shadow-[0_0_8px_currentColor]", killSwitchActive ? "bg-[#f43f5e] text-[#f43f5e]" : "bg-[#10b981] text-[#10b981] animate-pulse")} />
              <span className={cn("text-[10px] font-bold uppercase tracking-wider", killSwitchActive ? "text-[#f43f5e]" : "text-[#10b981]")}>
                {killSwitchActive ? 'Killed' : 'Connected'}
              </span>
            </div>
            <span className={cn("text-[10px] font-mono", latency > 100 ? (latency > 500 ? "text-[#f43f5e]" : "text-[#f59e0b]") : "text-[#7b859c]")}>{latency}ms</span>
          </div>
        </div>
      </aside>

      <main className="flex-1 flex flex-col min-w-0 relative z-10 w-full overflow-hidden">
        
        <header className="h-16 border-b border-white/5 bg-[rgba(3,5,10,0.6)] backdrop-blur-md flex items-center justify-between px-8 shrink-0">
          <div className="flex items-center gap-6">
             <div className="flex items-center gap-6">
                <div className="flex flex-col">
                  <span className="text-[9px] font-bold text-[#7b859c] uppercase tracking-wider">Equity</span>
                  <span className="text-lg font-mono font-bold text-white text-glow-blue">${equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</span>
                </div>
                <div className="w-px h-8 bg-white/10" />
                <div className="flex flex-col">
                  <span className="text-[9px] font-bold text-[#7b859c] uppercase tracking-wider">Free Balance</span>
                  <span className="text-[13px] font-mono font-bold text-white">${freeBalance.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</span>
                </div>
                <div className="w-px h-8 bg-white/10" />
                <div className="flex flex-col">
                  <span className="text-[9px] font-bold text-[#7b859c] uppercase tracking-wider">Unrealized PnL</span>
                  <span className={cn("text-[13px] font-mono font-bold", unrealizedPnl >= 0 ? "text-[#10b981] text-glow-green" : "text-[#f43f5e]")}>
                    {unrealizedPnl >= 0 ? '+' : ''}{unrealizedPnl.toFixed(2)}
                  </span>
                </div>
                <div className="w-px h-8 bg-white/10" />
                <div className="flex flex-col">
                  <span className="text-[9px] font-bold text-[#7b859c] uppercase tracking-wider">Session PnL</span>
                  <span className={cn("text-[13px] font-mono font-bold", sessionPnL >= 0 ? "text-[#10b981] text-glow-green" : "text-[#f43f5e]")}>
                    {sessionPnL >= 0 ? '+' : ''}{sessionPnL.toFixed(2)} ({sessionPnLPct.toFixed(2)}%)
                  </span>
                </div>
                <div className="w-px h-8 bg-white/10" />
                <div className="flex flex-col">
                  <span className="text-[9px] font-bold text-rose-500 uppercase tracking-wider">Margin Used</span>
                  <span className="text-[13px] font-mono font-bold text-white">{marginUsedPct.toFixed(1)}%</span>
                </div>
                <div className="w-px h-8 bg-white/10" />
                <div className="flex flex-col">
                  <span className="text-[9px] font-bold text-amber-500 uppercase tracking-wider">Daily Drawdown</span>
                  <span className="text-[13px] font-mono font-bold text-white">{dailyDrawdownPct.toFixed(2)}%</span>
                </div>
             </div>
          </div>
          
          <div className="flex items-center gap-4">
             <div className={cn("w-2 h-2 rounded-full", wsConnected ? "bg-[#10b981]" : "bg-[#f59e0b] animate-pulse")} title={wsConnected ? "WS Connected" : "WS Disconnected"} />
             
             <button
               onClick={() => {
                 if (recorderActive) {
                   apiStopDump().catch(console.error);
                 } else {
                   apiStartDump().catch(console.error);
                 }
               }}
               className={cn(
                 "px-4 py-2 text-[11px] font-black uppercase tracking-widest rounded-lg transition-all border",
                 recorderActive ? "bg-amber-500/20 text-amber-500 border-amber-500/50 shadow-[0_0_15px_rgba(245,158,11,0.2)] animate-pulse" : "bg-white/5 text-[#7b859c] border-white/10 hover:bg-white/10 hover:text-white"
               )}>
                {recorderActive ? "● Recording" : "○ Record Dump"}
             </button>

             <button
               onClick={() => { toggleKillSwitch(); apiToggleKillSwitch().catch(console.error); }}
               className={cn(
                 "px-6 py-2 text-[11px] font-black uppercase tracking-widest rounded-lg transition-all border shadow-[0_0_15px_rgba(244,63,94,0.15)]",
                 killSwitchActive ? "bg-[#f43f5e] text-white border-[#f43f5e] animate-pulse" : "bg-[#f43f5e]/10 text-[#f43f5e] border-[#f43f5e]/30 hover:bg-[#f43f5e] hover:text-white"
               )}>
                {killSwitchActive ? "RECOVER TRADING" : "KILL SWITCH"}
             </button>
          </div>
        </header>

        {/* Tab Content */}
        <div className="flex-1 overflow-y-auto w-full relative">
            
            {activeTab === 'market' && (
              <div className="absolute inset-0 grid grid-cols-12 gap-0 overflow-hidden">
                {/* LADDER (Left 30% roughly = ~4 cols) */}
                <div className="col-span-12 lg:col-span-4 border-r border-white/5 h-full overflow-hidden">
                   <Ladder ticker={activeTicker} />
                </div>

                {/* CENTER: Chart (approx 50% = ~6 cols) */}
                <div className="col-span-12 lg:col-span-6 border-r border-white/5 h-full flex flex-col relative">
                   <div className="h-12 border-b border-white/5 flex items-center px-4 justify-between bg-black/40">
                      <div className="flex items-center gap-4">
                        {strategyStates.slice(0, 6).map(s => (
                          <button key={s.ticker} onClick={() => { setActiveTicker(s.ticker); apiSelectTicker(s.ticker).catch(console.error); }} className={cn("text-xs font-bold font-mono px-3 py-1.5 rounded-lg transition-colors", activeTicker === s.ticker ? "bg-white/10 text-white" : "text-[#7b859c] hover:text-[#c0c7d4]")}>
                            {(s.ticker.split(':')[1] ?? s.ticker).replace('.p', '')}
                          </button>
                        ))}
                      </div>
                   </div>
                   <div className="flex-1 relative min-h-0">
                      <div className="absolute inset-0">
                         <CandlestickChart ticker={activeTicker} />
                      </div>
                      
                      {/* Floating open positions for this ticker */}
                      <div className="absolute top-4 left-4 z-10 flex flex-col gap-2 pointer-events-none">
                         {trades.filter(t => t.ticker === activeTicker.split(':')[1]).map(t => (
                           <div key={t.id} className="bg-[rgba(3,5,10,0.8)] backdrop-blur-md border border-[#58a6ff]/30 p-3 rounded-xl min-w-[200px]">
                              <div className="flex justify-between items-center mb-1">
                                <span className={cn("text-[9px] font-black uppercase", t.side === 'LONG' ? "text-[#10b981]" : "text-[#f43f5e]")}>{t.side}</span>
                                <span className="text-[10px] text-[#7b859c] font-mono">Entry: {t.entry_price}</span>
                              </div>
                              <div className="flex justify-between items-end">
                                <span className="text-xs text-white font-bold">{t.strategy}</span>
                                <span className={cn("text-sm font-mono font-bold", t.pnl_usd >= 0 ? "text-[#10b981]" : "text-[#f43f5e]")}>
                                  {t.pnl_usd >= 0 ? '+' : ''}{t.pnl_usd.toFixed(2)}
                                </span>
                              </div>
                           </div>
                         ))}
                      </div>
                   </div>
                </div>

                {/* RIGHT: Intelligence & Mini-charts (approx 20% = ~2 cols) */}
                <div className="col-span-12 lg:col-span-2 bg-black/20 h-full flex flex-col overflow-y-auto no-scrollbar">
                   <div className="p-4 border-b border-white/5 font-bold text-xs uppercase tracking-wider text-[#7b859c]">Analytics</div>
                   
                   <div className="p-4 space-y-6 border-b border-white/5">
                      <div>
                        <div className="text-[9px] text-[#7b859c] uppercase tracking-widest font-bold mb-1">Spread (BPS)</div>
                        <div className="h-10 w-full rounded overflow-hidden">
                           <CanvasMiniChart data={spreadArr} color="#f59e0b" />
                        </div>
                      </div>
                      <div>
                        <div className="text-[9px] text-[#7b859c] uppercase tracking-widest font-bold mb-1">Tape Aggression</div>
                        <div className="h-10 w-full rounded overflow-hidden">
                           <CanvasMiniChart data={aggressionArr} color="#58a6ff" />
                        </div>
                      </div>
                      <div>
                        <div className="text-[9px] text-[#7b859c] uppercase tracking-widest font-bold mb-1">Volume 1m</div>
                        <div className="h-10 w-full rounded overflow-hidden">
                           <CanvasMiniChart data={volArr} color="#8b5cf6" />
                        </div>
                      </div>
                   </div>

                   <TradingPanel />

                   <div className="flex-1 flex flex-col min-h-0">
                     <div className="p-4 border-b border-white/5 font-bold text-xs uppercase tracking-wider text-white bg-black/40 sticky top-0">Realtime Signals</div>
                     <div className="flex-1 overflow-y-auto space-y-[1px]">
                        {signals.map(s => (
                          <div key={`${s.id}-${s.timestamp}`} className={cn("p-4 bg-white/[0.02] border-l-2 hover:bg-white/[0.05] cursor-default transition-colors",
                            (s.side === 'BUY' || s.side === 'LONG') ? "border-[#10b981]" : "border-[#f43f5e]",
                            s.conf >= 0.8 && "shadow-[inset_0_0_20px_rgba(16,185,129,0.05)]"
                          )}>
                             <div className="flex gap-2 justify-between items-start mb-1">
                                <span className={cn("text-[9px] font-black uppercase", (s.side === 'BUY' || s.side === 'LONG') ? "text-[#10b981]" : "text-[#f43f5e]")}>{s.side}</span>
                                <span className="text-[9px] font-mono text-[#7b859c]">{(s.ticker.split(':')[1] ?? s.ticker).replace('.p','')}</span>
                             </div>
                             <div className="flex items-center gap-1.5 mb-0.5">
                               <div className="text-xs font-bold text-white leading-tight">{s.type}</div>
                               {s.count && s.count > 1 && <span className="text-[#8b5cf6] text-[9px] font-bold">x{s.count}</span>}
                               {s.conf >= 0.8 && (
                                 <span className="text-[9px] font-mono font-bold text-[#10b981] bg-[#10b981]/10 px-1 rounded">
                                   {Math.round(s.conf * 100)}%
                                 </span>
                               )}
                               {s.conf >= 0.5 && s.conf < 0.8 && (
                                 <span className="text-[9px] font-mono text-[#f59e0b]">
                                   {Math.round(s.conf * 100)}%
                                 </span>
                               )}
                             </div>
                             <div className="text-[10px] text-[#7b859c] leading-tight">{s.text}</div>
                          </div>
                        ))}
                     </div>
                   </div>
                </div>
              </div>
            )}

            {/* COMMAND CENTER */}
            {activeTab === 'command' && (
              <div className="h-full flex flex-col">
                <div className="flex-1 overflow-y-auto min-h-0">
                  <CommandCenter equityHistory={equityHistory} sparkData={sparkData} sparkData2={sparkData} />
                </div>
                <CommandInput />
              </div>
            )}

            {/* PORTFOLIO & RISK */}
            {activeTab === 'portfolio' && (
              <div className="max-w-[1400px] mx-auto space-y-6 p-8">
                 <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
                    {trades.map(t => (
                      <div key={t.id} className={cn("glass-card p-6 relative overflow-hidden group transition-all", killSwitchActive && "animate-pulse border-[#f43f5e] bg-[#f43f5e]/10")}>
                         <div className="flex items-center justify-between mb-4">
                            <div className="flex items-center gap-4">
                               <div className={cn("px-3 py-1.5 rounded-lg text-[11px] font-black uppercase text-center min-w-[70px]", t.side==='LONG' ? "bg-[#10b981]/15 text-[#10b981]" : "bg-[#f43f5e]/15 text-[#f43f5e]")}>
                                 {t.side}
                               </div>
                               <div>
                                  <div className="text-xl font-bold text-white max-w-[150px] truncate">{t.ticker}</div>
                                  <div className="text-[10px] text-[#7b859c] font-bold">{t.strategy}</div>
                               </div>
                            </div>
                            <div className="text-right">
                               <div className="text-[10px] font-bold text-[#7b859c] uppercase mb-1">Unrealized PnL</div>
                               <div className={cn("text-2xl font-mono font-black", (t.pnl_usd || 0) >= 0 ? "text-[#10b981]" : "text-[#f43f5e]")}>
                                 {(t.pnl_usd || 0) >= 0 ? '+' : ''}{(t.pnl_usd || 0).toFixed(2)}
                               </div>
                            </div>
                         </div>
                         
                         <div className="mt-8 relative">
                           <div className="flex justify-between text-[9px] font-bold text-[#7b859c] uppercase mb-2">
                             <span>SL (-1.5%)</span>
                             <span>Target TP (+3.0%)</span>
                           </div>
                           <div className="w-full h-1.5 bg-black/40 rounded-full overflow-hidden border border-white/5 relative">
                             {/* Progress representing distance to TP */}
                             <div className={cn("absolute left-0 top-0 bottom-0", t.side==='LONG'?"bg-[#10b981]":"bg-[#f43f5e]")} style={{ width: `${t.side === 'LONG' ? Math.max(0, Math.min(100, ((t.mark_price - t.entry_price) / (t.entry_price * 0.03)) * 100)) : Math.max(0, Math.min(100, ((t.entry_price - t.mark_price) / (t.entry_price * 0.03)) * 100))}%` }} />
                           </div>
                         </div>

                         <div className="mt-6 flex justify-end">
                            <button onClick={() => apiSendCommand('close ' + t.ticker).catch(console.error)} className="px-6 py-2 bg-[#f43f5e]/15 text-[#f43f5e] hover:bg-[#f43f5e] hover:text-white rounded-lg text-[10px] font-black uppercase tracking-widest transition-all">
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

            {/* JOURNAL TAB */}
            {activeTab === 'journal' && (
              <div className="absolute inset-0 flex flex-col p-8 overflow-hidden gap-6">
                <div className="flex items-center gap-4 mb-4">
                  <div className="w-10 h-10 bg-gradient-to-br from-[#8b5cf6] to-[#6d28d9] rounded-xl flex items-center justify-center shadow-[0_0_15px_rgba(139,92,246,0.4)]">
                    <Layers size={20} className="text-white" />
                  </div>
                  <div>
                    <h1 className="text-xl font-black text-white tracking-tight">Trade Journal</h1>
                    <div className="text-xs font-bold text-[#7b859c] uppercase tracking-wider">Executed Signals & History</div>
                  </div>
                </div>
                <div className="flex-1 glass-card overflow-hidden flex flex-col">
                  <Journal />
                </div>
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
