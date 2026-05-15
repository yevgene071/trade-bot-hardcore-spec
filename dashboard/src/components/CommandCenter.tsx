import React, { useState } from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { Activity, ShieldAlert, Layers, TrendingUp } from 'lucide-react';
import { cn } from '../lib/utils';
import { ResponsiveContainer, AreaChart, Area } from 'recharts';
import { StatCard, ConditionBar } from './ui/shared';
import { StrategyReadyState } from '../types';

export function CommandCenter({ equityHistory, sparkData, sparkData2 }: any) {
  const store = useTradeStore();
  const [selectedHeatmapTicker, setSelectedHeatmapTicker] = useState<string | null>(null);

  const selectedStrategyState = store.strategyStates.find(s => s.ticker === selectedHeatmapTicker) || store.strategyStates[0];

  return (
    <div className="max-w-[1400px] mx-auto space-y-6 p-8">
      <div className="grid grid-cols-1 md:grid-cols-3 lg:grid-cols-6 gap-4">
        <StatCard label="Total Equity" value={`${store.equity.toFixed(2)}`} sub="1.0x" icon={Activity} color="blue" data={sparkData} />
        <StatCard label="Daily Realized PnL" value={`+${store.sessionPnL.toFixed(2)}`} sub={`+${store.sessionPnLPct.toFixed(2)}%`} icon={TrendingUp} color="emerald" data={sparkData2} />
        <StatCard label="Margin Exposure" value={`${store.marginUsedPct.toFixed(1)}%`} sub="SAFE" icon={Layers} color="slate" glow={false} />
        <StatCard label="Drawdown" value={`${store.dailyDrawdownPct.toFixed(2)}%`} sub="NORMAL" icon={ShieldAlert} color="amber" glow={false} />
        <StatCard label="Total Trades" value={`${store.risk.totalTradesToday}`} sub={`${store.risk.consecutiveLosses} loss streak`} icon={Activity} color="slate" glow={false} />
        <StatCard label="Total Exposure" value={`${store.risk.exposurePct.toFixed(1)}%`} sub="RISK" icon={ShieldAlert} color="amber" glow={false} />
      </div>
      
      <div className="grid grid-cols-1 xl:grid-cols-3 gap-6">
        <div className="xl:col-span-2 glass-card p-6 flex flex-col">
           <div className="flex items-center justify-between mb-4">
             <h2 className="text-sm font-bold text-white tracking-tight uppercase">Equity Curve (24h)</h2>
           </div>
           <div className="h-[250px]">
              <ResponsiveContainer width="100%" height="100%">
                  <AreaChart data={equityHistory}>
                    <defs>
                      <linearGradient id="eqGrad" x1="0" y1="0" x2="0" y2="1">
                        <stop offset="5%" stopColor="#58a6ff" stopOpacity={0.2}/>
                        <stop offset="95%" stopColor="#58a6ff" stopOpacity={0}/>
                      </linearGradient>
                    </defs>
                    <Area type="monotone" dataKey="val" stroke="#58a6ff" strokeWidth={2} fill="url(#eqGrad)" isAnimationActive={false} />
                  </AreaChart>
              </ResponsiveContainer>
           </div>
        </div>

        <div className="glass-card p-6 flex flex-col">
           <h2 className="text-sm font-bold text-white tracking-tight uppercase mb-4">Strategy Heatmap</h2>
           <div className="flex gap-6 flex-1 mt-4">
             <div className="grid grid-cols-2 gap-2 flex-1 place-content-start">
                {store.strategyStates.map(s => {
                   const isReady = s.ready_state === StrategyReadyState.Ready || s.ready_state === StrategyReadyState.Trading;
                   const isWarming = s.ready_state === StrategyReadyState.Warming || s.ready_state === StrategyReadyState.Planning;
                   const isSelected = selectedHeatmapTicker === s.ticker;
                   
                   return (
                     <div 
                       key={s.ticker} 
                       onClick={() => setSelectedHeatmapTicker(s.ticker)}
                       className={cn(
                         "p-3 rounded-lg flex flex-col justify-center items-center cursor-pointer transition-colors border",
                         isSelected && "ring-1 ring-white/50",
                         isReady ? "bg-[#10b981]/10 border-[#10b981]/30 hover:bg-[#10b981]/20" : 
                         isWarming ? "bg-[#f59e0b]/10 border-[#f59e0b]/30 hover:bg-[#f59e0b]/20" : 
                         "bg-white/[0.02] border-white/5 hover:bg-white/[0.05]"
                       )}
                     >
                       <div className="text-[10px] font-bold text-white mb-1">{(s.ticker.split(':')[1] ?? s.ticker).replace('.p','')}</div>
                       <div className={cn("text-[9px] font-mono", isReady ? "text-[#10b981]" : isWarming ? "text-[#f59e0b]" : "text-[#7b859c]")}>
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
