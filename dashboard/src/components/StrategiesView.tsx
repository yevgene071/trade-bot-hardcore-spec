import React from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { StrategyReadyState } from '../types';
import { apiSendCommand } from '../transport/api';
import { cn } from '../lib/utils';

function fmtCondVal(v: number, unit?: string): string {
  if (!Number.isFinite(v)) return '—';
  if (Math.abs(v) < 1e-9) return '0.00';
  if (unit === 'count') return v.toFixed(0);
  if (Math.abs(v) < 0.01 && v !== 0) return v.toFixed(4);
  return v.toFixed(2);
}

const STATE_LABEL: Record<number, { label: string; color: string }> = {
  [StrategyReadyState.Cold]:    { label: 'Cold',    color: 'text-[#3c4e62] border-[#141e30] bg-white/5' },
  [StrategyReadyState.Warming]: { label: 'Warming', color: 'text-[#c08828] border-[#c08828]/30 bg-[#c08828]/10' },
  [StrategyReadyState.Ready]:   { label: 'Ready',   color: 'text-[#1a9448] border-[#1a9448]/30 bg-[#1a9448]/10' },
  [StrategyReadyState.Planning]:{ label: 'Planning',color: 'text-[#8b5cf6] border-[#8b5cf6]/30 bg-[#8b5cf6]/10' },
  [StrategyReadyState.Trading]: { label: 'Trading', color: 'text-[#3478b8] border-[#3478b8]/30 bg-[#3478b8]/10' },
  [StrategyReadyState.Cooldown]:{ label: 'Cooldown',color: 'text-[#c08828] border-[#c08828]/30 bg-[#c08828]/5' },
};

export function StrategiesView() {
  const strategyStates = useTradeStore(s => s.strategyStates);

  const sendCmd = (cmd: string) => apiSendCommand(cmd).catch(console.error);

  return (
    <div className="max-w-[1200px] mx-auto p-8 space-y-4">
      <div className="flex items-center gap-4 mb-6">
        <div className="w-10 h-10 bg-gradient-to-br from-[#8b5cf6] to-[#6d28d9] rounded-xl flex items-center justify-center shadow-[0_0_15px_rgba(139,92,246,0.4)]">
          <span className="text-white text-lg font-black">S</span>
        </div>
        <div>
          <h1 className="text-xl font-black text-white tracking-tight">Strategies</h1>
          <div className="text-xs font-bold text-[#7b859c] uppercase tracking-wider">{strategyStates.length} active instruments</div>
        </div>
      </div>

      {strategyStates.map((s, idx) => {
        const stateInfo = STATE_LABEL[s.ready_state] || STATE_LABEL[StrategyReadyState.Cold];
        const tickerShort = s.ticker.split(':')[1]?.replace('.p', '').replace('.m', '') || s.ticker;

        return (
          <div key={`strat-${idx}-${s.ticker}`} className="glass-card p-6">
            <div className="flex items-center justify-between mb-4">
              <div className="flex items-center gap-4">
                <div className="text-lg font-black text-white">{tickerShort}</div>
                <div className="text-xs font-bold text-[#7b859c]">{s.strategy_name}</div>
                <span className={cn("text-[9px] font-black px-2 py-0.5 rounded border uppercase tracking-wider", stateInfo.color)}>
                  {stateInfo.label}
                </span>
              </div>
              <div className="flex items-center gap-2">
                <button
                  onClick={() => sendCmd(`strategy enable ${s.ticker} ${s.strategy_name}`)}
                  className="px-3 py-1.5 text-[9px] font-black uppercase tracking-wider rounded border border-[#1a9448]/30 text-[#1a9448] bg-[#1a9448]/10 hover:bg-[#1a9448] hover:text-white transition-colors"
                >Enable</button>
                <button
                  onClick={() => sendCmd(`strategy disable ${s.ticker} ${s.strategy_name}`)}
                  className="px-3 py-1.5 text-[9px] font-black uppercase tracking-wider rounded border border-[#ae2c2c]/30 text-[#ae2c2c] bg-[#ae2c2c]/10 hover:bg-[#ae2c2c] hover:text-white transition-colors"
                >Disable</button>
              </div>
            </div>

            {/* Readiness bar */}
            <div className="mb-4">
              <div className="flex justify-between text-[9px] font-bold text-[#7b859c] uppercase mb-1">
                <span>Readiness</span>
                <span>{(Math.round(s.readiness_pct * 10) / 10).toFixed(1)}%</span>
              </div>
              <div className="w-full h-1.5 bg-black/40 rounded-full overflow-hidden border border-white/5">
                <div
                  className={cn("h-full rounded-full transition-all duration-500",
                    s.readiness_pct >= 80 ? "bg-[#1a9448]" : s.readiness_pct >= 40 ? "bg-[#c08828]" : "bg-[#3c4e62]"
                  )}
                  style={{ width: `${s.readiness_pct}%` }}
                />
              </div>
            </div>

            {/* Conditions */}
            {s.conditions && s.conditions.length > 0 && (
              <div className="space-y-1.5">
                {s.conditions.map(c => (
                  <div key={c.name} className="flex items-center gap-3 text-[10px]">
                    <span className={cn("w-3 h-3 rounded-full shrink-0", c.met ? "bg-[#1a9448]" : "bg-[#ae2c2c]/50")} />
                    <span className="text-[#7b859c] w-[80px] shrink-0">{c.name}</span>
                    <span className="font-mono text-white">{fmtCondVal(c.current, c.unit)}</span>
                    <span className="text-[#7b859c]">/</span>
                    <span className="font-mono text-[#7b859c]">{fmtCondVal(c.target, c.unit)} {c.unit}</span>
                  </div>
                ))}
              </div>
            )}

            {/* Reject reason */}
            {s.last_reject_reason && (
              <div className="mt-3 text-[9px] font-mono text-[#c08828] bg-[#c08828]/5 border border-[#c08828]/10 rounded px-3 py-2">
                Last reject: {s.last_reject_reason}
                {s.seconds_since_last_reject !== undefined && (
                  <span className="text-[#7b859c] ml-2">({Math.round(s.seconds_since_last_reject)}s ago)</span>
                )}
              </div>
            )}
            
            {/* Signals in last 60s */}
            {s.signals_last_60s !== undefined && (
              <div className="mt-2 text-[9px] font-bold text-[#7b859c] uppercase">
                Signals (last 60s): <span className="text-white font-mono">{s.signals_last_60s}</span>
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}
