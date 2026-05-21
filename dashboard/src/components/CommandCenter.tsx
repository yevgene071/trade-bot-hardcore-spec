import React, { useState } from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { Activity, ShieldAlert, Layers, TrendingUp } from 'lucide-react';
import { cn } from '../lib/utils';
import { ResponsiveContainer, AreaChart, Area } from 'recharts';
import { StatCard, ConditionBar } from './ui/shared';
import { StrategyReadyState } from '../types';

export function CommandCenter({ equityHistory, sparkData }: any) {
  const store = useTradeStore();
  const [selectedHeatmapTicker, setSelectedHeatmapTicker] = useState<string | null>(null);

  const selectedStrategyState = store.strategyStates.find(s => s.ticker === selectedHeatmapTicker) || store.strategyStates[0];

  // Derive risk badges from real values instead of hardcoded SAFE/NORMAL/RISK.
  const band = (v: number, warn: number, danger: number) =>
    v >= danger ? { t: 'DANGER', c: 'rose' as const }
    : v >= warn ? { t: 'ELEVATED', c: 'amber' as const }
    : { t: 'SAFE', c: 'emerald' as const };
  const marginB = band(store.marginUsedPct, 50, 80);
  const expB = band(store.risk.exposurePct, 60, 90);
  const dd = Math.abs(store.dailyDrawdownPct);
  const ddB = dd >= 5 ? { t: 'CRITICAL', c: 'rose' as const }
    : dd >= 2 ? { t: 'ELEVATED', c: 'amber' as const }
    : { t: 'NORMAL', c: 'emerald' as const };

  const pnl = store.sessionPnL;
  const pnlStr = `${pnl >= 0 ? '+' : '-'}$${Math.abs(pnl).toFixed(2)}`;
  const pnlPctStr = `${store.sessionPnLPct >= 0 ? '+' : ''}${store.sessionPnLPct.toFixed(2)}%`;

  return (
    <div className="max-w-[1400px] mx-auto space-y-6 p-8">
      <div className="grid grid-cols-1 md:grid-cols-3 lg:grid-cols-6 gap-4">
        <StatCard label="Total Equity" value={`$${store.equity.toFixed(2)}`} icon={Activity} color="blue" data={sparkData} />
        <StatCard label="Daily Realized PnL" value={pnlStr} sub={pnlPctStr} icon={TrendingUp} color={pnl >= 0 ? 'emerald' : 'rose'} />
        <StatCard label="Margin Exposure" value={`${store.marginUsedPct.toFixed(1)}%`} sub={marginB.t} icon={Layers} color={marginB.c} glow={false} />
        <StatCard label="Drawdown" value={`${store.dailyDrawdownPct.toFixed(2)}%`} sub={ddB.t} icon={ShieldAlert} color={ddB.c} glow={false} />
        <StatCard label="Total Trades" value={`${store.risk.totalTradesToday}`} sub={`${store.risk.consecutiveLosses} loss streak`} icon={Activity} color="slate" glow={false} />
        <StatCard label="Total Exposure" value={`${store.risk.exposurePct.toFixed(1)}%`} sub={expB.t} icon={ShieldAlert} color={expB.c} glow={false} />
      </div>
      
      <div className="grid grid-cols-1 xl:grid-cols-3 gap-6">
        <div className="xl:col-span-2 glass-card p-6 flex flex-col">
           <div className="flex items-center justify-between mb-4">
             <h2 className="text-sm font-bold text-white tracking-tight uppercase">Equity Curve (session)</h2>
           </div>
           <div className="h-[250px]">
              {equityHistory && equityHistory.length > 0 ? (
                <ResponsiveContainer width="100%" height="100%" minHeight={250}>
                    <AreaChart data={equityHistory} margin={{ top: 0, right: 0, left: 0, bottom: 0 }}>
                      <defs>
                        <linearGradient id="eqGrad" x1="0" y1="0" x2="0" y2="1">
                          <stop offset="5%" stopColor="#1a9448" stopOpacity={0.18}/>
                          <stop offset="95%" stopColor="#1a9448" stopOpacity={0}/>
                        </linearGradient>
                      </defs>
                      <Area type="monotone" dataKey="val" stroke="#1a9448" strokeWidth={1.5} fill="url(#eqGrad)" isAnimationActive={false} />
                    </AreaChart>
                </ResponsiveContainer>
              ) : (
                <div className="flex items-center justify-center h-full text-[#3c4e62] text-[10px] font-mono">Awaiting equity data…</div>
              )}
           </div>
        </div>

        <div className="glass-card p-6 flex flex-col">
           <h2 className="text-sm font-bold text-white tracking-tight uppercase mb-4">Strategy Heatmap</h2>
           <div className="flex gap-6 flex-1 mt-4">
             <div className="grid grid-cols-2 gap-2 flex-1 place-content-start">
                {store.strategyStates.map((s, idx) => {
                   const isReady = s.ready_state === StrategyReadyState.Ready || s.ready_state === StrategyReadyState.Trading;
                   const isWarming = s.ready_state === StrategyReadyState.Warming || s.ready_state === StrategyReadyState.Planning;
                   const isSelected = selectedHeatmapTicker === s.ticker;

                   return (
                     <div
                       key={`hm-${idx}-${s.ticker}`} 
                       onClick={() => setSelectedHeatmapTicker(s.ticker)}
                       className={cn(
                         "p-3 rounded-lg flex flex-col justify-center items-center cursor-pointer transition-colors border",
                         isSelected && "ring-1 ring-white/50",
                         isReady ? "bg-[#1a9448]/10 border-[#1a9448]/30 hover:bg-[#1a9448]/20" : 
                         isWarming ? "bg-[#c08828]/10 border-[#c08828]/30 hover:bg-[#c08828]/20" : 
                         "bg-white/[0.02] border-[#141e30] hover:bg-white/[0.05]"
                       )}
                     >
                       <div className="text-[10px] font-bold text-white mb-1">{(s.ticker.split(':')[1] ?? s.ticker).replace('.p','')}</div>                        <div className={cn("text-[9px] font-mono", isReady ? "text-[#1a9448]" : isWarming ? "text-[#c08828]" : "text-[#3c4e62]")}>
                         {s.readiness_pct}%
                       </div>
                     </div>
                   );
                })}
             </div>
             
             {selectedStrategyState ? (
                <div className="w-[200px] border-l border-white/5 pl-6 space-y-3 shrink-0">
                  <div className="flex justify-between items-end mb-2">
                     <span className="text-[9px] font-bold text-[#7b859c] uppercase tracking-widest pl-1">Condition Trace</span>
                     <span className="text-[9px] font-bold text-white truncate max-w-[80px]" title={selectedStrategyState.ticker}>{(selectedStrategyState.ticker.split(':')[1] ?? selectedStrategyState.ticker).replace('.p','')}</span>
                  </div>
                  {selectedStrategyState.conditions?.length ? selectedStrategyState.conditions.map((c: any) => <ConditionBar key={c.name} {...c} />) : (
                    <div className="text-[10px] text-[#7b859c] text-center mt-10">No Trace Data</div>
                  )}
                </div>
             ) : (
                <div className="w-[200px] border-l border-white/5 pl-6 flex items-center justify-center shrink-0">
                   <span className="text-[10px] text-[#7b859c]">Select a strategy</span>
                </div>
             )}
             
           </div>
        </div>
      </div>
    </div>
  );
}
