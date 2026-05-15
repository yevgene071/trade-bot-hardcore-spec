import React from 'react';
import { ResponsiveContainer, AreaChart, Area } from 'recharts';
import { cn } from '../../lib/utils';

export function StatCard({ label, value, sub, icon: Icon, color = "blue", glow = true, data = null }: any) {
  const cMap: any = {
    blue: { icon: "text-[#58a6ff] bg-[#58a6ff]/10", border: "border-[#58a6ff]/20", glow: "shadow-[0_0_20px_rgba(88,166,255,0.15)]", chart: "#58a6ff" },
    emerald: { icon: "text-[#10b981] bg-[#10b981]/10", border: "border-[#10b981]/20", glow: "shadow-[0_0_20px_rgba(16,185,129,0.15)]", chart: "#10b981" },
    amber: { icon: "text-[#f59e0b] bg-[#f59e0b]/10", border: "border-[#f59e0b]/20", glow: "shadow-[0_0_20px_rgba(245,158,11,0.15)]", chart: "#f59e0b" },
    rose: { icon: "text-[#f43f5e] bg-[#f43f5e]/10", border: "border-[#f43f5e]/20", glow: "shadow-[0_0_20px_rgba(244,63,94,0.15)]", chart: "#f43f5e" },
    slate: { icon: "text-[#7b859c] bg-[#7b859c]/10", border: "border-[#7b859c]/20", glow: "", chart: "#7b859c" }
  };

  const style = cMap[color];

  return (
    <div className={cn("glass-card p-5 group cursor-default relative overflow-hidden", glow && style.glow)}>
      <div className="absolute -top-10 -right-10 w-24 h-24 bg-white/[0.02] rounded-full blur-2xl z-0" />
      {data && (
        <div className="absolute inset-0 top-1/2 z-0 opacity-10 group-hover:opacity-30 transition-opacity duration-500">
          <ResponsiveContainer width="100%" height="100%">
             <AreaChart data={data}>
                <Area type="monotone" dataKey="v" stroke={style.chart} fill={style.chart} strokeWidth={2} isAnimationActive={false} />
             </AreaChart>
          </ResponsiveContainer>
        </div>
      )}
      <div className="flex items-start justify-between mb-2 relative z-10">
        <div className={cn("p-2 rounded-lg transition-transform duration-500 group-hover:scale-110", style.icon, style.border, "border")}>
          <Icon size={16} />
        </div>
        {sub && <div className={cn("text-[9px] font-black px-2 py-0.5 rounded font-mono border", style.icon, style.border)}>{sub}</div>}
      </div>
      <div className="text-[10px] font-bold text-[#7b859c] uppercase tracking-widest mb-1 relative z-10">{label}</div>
      <div className="text-xl font-black text-white tracking-tight tabular-nums relative z-10">{value}</div>
    </div>
  );
}

export function ConditionBar({ name, current, target, met, unit }: any) {
  const pct = Math.min(100, (Math.abs(Number(current)) / Math.max(1, Math.abs(Number(target)))) * 100);
  return (
    <div className={cn("px-4 py-3 rounded-xl border transition-all", met ? "bg-[#10b981]/5 border-[#10b981]/15" : "bg-white/[0.02] border-white/5")}>
       <div className="flex justify-between items-center mb-1.5">
          <span className="text-[10px] font-bold text-[#c0c7d4] tracking-wide">{name}</span>
          <span className={cn("text-[10px] font-mono font-bold", met ? "text-[#10b981]" : "text-[#7b859c]")}>
            {typeof current === 'number' ? current.toFixed(1) : current}{unit} <span className="text-white/20">/</span> {target}{unit}
          </span>
       </div>
       <div className="w-full h-1.5 bg-[#0f1525] rounded-full overflow-hidden border border-white/5">
          <div
            className={cn("h-full transition-all duration-500", met ? "bg-[#10b981] shadow-[0_0_10px_#10b981]" : "bg-[#f59e0b]")}
            style={{ width: `${pct}%` }}
          />
       </div>
    </div>
  );
}
