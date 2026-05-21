import React from 'react';
import { ResponsiveContainer, AreaChart, Area } from 'recharts';
import { cn } from '../../lib/utils';

export function StatCard({ label, value, sub, icon: Icon, color = "blue", glow = false, data = null }: any) {
  const cMap: any = {
    blue: { icon: "text-[#3478b8] bg-[#3478b8]/10", border: "border-[#3478b8]/20", chart: "#3478b8" },
    emerald: { icon: "text-[#1a9448] bg-[#1a9448]/10", border: "border-[#1a9448]/20", chart: "#1a9448" },
    amber: { icon: "text-[#c08828] bg-[#c08828]/10", border: "border-[#c08828]/20", chart: "#c08828" },
    rose: { icon: "text-[#ae2c2c] bg-[#ae2c2c]/10", border: "border-[#ae2c2c]/20", chart: "#ae2c2c" },
    slate: { icon: "text-[#3c4e62] bg-[#3c4e62]/10", border: "border-[#3c4e62]/20", chart: "#3c4e62" }
  };

  const style = cMap[color];

  return (
    <div className="glass-card p-5 cursor-default relative overflow-hidden min-h-[80px]">
      {data && data.length > 0 && (
        <div className="absolute inset-0 z-0 opacity-8 pointer-events-none">
          <div className="w-full h-full min-h-[48px]">
            <ResponsiveContainer width="100%" height="100%" minHeight={48}>
               <AreaChart data={data} margin={{ top: 0, right: 0, left: 0, bottom: 0 }}>
                  <Area type="monotone" dataKey="v" stroke={style.chart} fill={style.chart} strokeWidth={1.5} isAnimationActive={false} />
               </AreaChart>
            </ResponsiveContainer>
          </div>
        </div>
      )}
      <div className="flex items-start justify-between mb-2 relative z-10">
        <div className={cn("p-2 rounded-lg", style.icon, style.border, "border")}>
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
    <div className={cn("px-4 py-3 rounded-xl border", met ? "bg-[#1a9448]/5 border-[#1a9448]/15" : "bg-white/[0.02] border-[#141e30]")}>
       <div className="flex justify-between items-center mb-1.5">
          <span className="text-[10px] font-bold text-[#b0c0d4] tracking-wide">{name}</span>
          <span className={cn("text-[10px] font-mono font-bold", met ? "text-[#1a9448]" : "text-[#3c4e62]")}>
            {typeof current === 'number' ? current.toFixed(1) : current}{unit} <span className="text-white/20">/</span> {target}{unit}
          </span>
       </div>
       <div className="w-full h-1.5 bg-black/40 rounded-full overflow-hidden border border-[#141e30]">
          <div
            className={cn("h-full", met ? "bg-[#1a9448]" : "bg-[#c08828]")}
            style={{ width: `${pct}%` }}
          />
       </div>
    </div>
  );
}
