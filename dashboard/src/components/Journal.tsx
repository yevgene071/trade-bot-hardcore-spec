import React, { useMemo } from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { cn } from '../lib/utils';
import { format } from 'date-fns';

export function Journal() {
  const serverEntries = useTradeStore(state => state.journalEntries);

  const journalEntries = useMemo(() => {
    if (serverEntries.length > 0) {
      return serverEntries.map(e => ({
        id: e.id,
        ticker: e.ticker,
        side: e.side,
        entryTime: e.entryTimeMs,
        exitTime: e.exitTimeMs,
        entryPrice: e.entryPrice,
        exitPrice: e.exitPrice,
        size: e.size,
        pnl: e.pnlUsd,
        strategy: e.strategy,
      }));
    }
    return Array.from({ length: 20 }).map((_, i) => ({
      id: `trd-${i}`,
      ticker: ['BTCUSDT', 'ETHUSDT', 'SOLUSDT', 'TONUSDT'][Math.floor(Math.random() * 4)],
      side: Math.random() > 0.5 ? 'LONG' : 'SHORT',
      entryTime: Date.now() - Math.random() * 86400000,
      exitTime: Date.now() - Math.random() * 3600000,
      entryPrice: 60000 + Math.random() * 5000,
      exitPrice: 60000 + Math.random() * 5000,
      size: Math.random() * 2 + 0.1,
      pnl: (Math.random() - 0.4) * 500,
      strategy: ['Impulse', 'MeanRev', 'Grid', 'Breakout'][Math.floor(Math.random() * 4)]
    })).sort((a, b) => b.exitTime - a.exitTime);
  }, [serverEntries]);

  return (
    <div className="h-full flex flex-col bg-black/40">
      <div className="p-3 border-b border-white/5 flex justify-between items-center bg-black/40 shrink-0">
        <div className="text-xs font-bold text-white uppercase tracking-wider">
          Trade Journal
        </div>
      </div>
      <div className="flex-1 overflow-auto no-scrollbar">
        <table className="w-full text-left border-collapse text-[11px] font-mono whitespace-nowrap">
          <thead className="sticky top-0 bg-[#0a0d14] z-10">
            <tr className="text-[#6b7280] uppercase tracking-wider">
              <th className="py-2 px-3 font-medium">Time</th>
              <th className="py-2 px-3 font-medium">Pair</th>
              <th className="py-2 px-3 font-medium">Side</th>
              <th className="py-2 px-3 font-medium">Size</th>
              <th className="py-2 px-3 font-medium">Entry</th>
              <th className="py-2 px-3 font-medium">Exit</th>
              <th className="py-2 px-3 font-medium">Strategy</th>
              <th className="py-2 px-3 text-right font-medium">PnL</th>
            </tr>
          </thead>
          <tbody>
            {journalEntries.map((entry) => {
              const isProfit = entry.pnl >= 0;
              return (
                <tr key={entry.id} className="border-b border-white/5 hover:bg-white/[0.02] transition-colors">
                  <td className="py-2 px-3 text-[#7b859c]">
                    {format(entry.exitTime, 'HH:mm:ss')}
                  </td>
                  <td className="py-2 px-3 font-bold text-white">
                    {entry.ticker}
                  </td>
                  <td className="py-2 px-3">
                    <span className={cn(
                      "px-1.5 py-0.5 rounded text-[9px] font-black tracking-widest",
                      entry.side === 'LONG' ? "bg-[#10b981]/20 text-[#10b981]" : "bg-[#f43f5e]/20 text-[#f43f5e]"
                    )}>
                      {entry.side}
                    </span>
                  </td>
                  <td className="py-2 px-3 text-white">
                    {entry.size.toFixed(2)}
                  </td>
                  <td className="py-2 px-3 text-[#7b859c]">
                    {entry.entryPrice.toFixed(1)}
                  </td>
                  <td className="py-2 px-3 text-[#7b859c]">
                    {entry.exitPrice.toFixed(1)}
                  </td>
                  <td className="py-2 px-3 text-[#7b859c]">
                    {entry.strategy}
                  </td>
                  <td className={cn(
                    "py-2 px-3 text-right font-bold",
                    isProfit ? "text-[#10b981] text-glow-green" : "text-[#f43f5e]"
                  )}>
                    {isProfit ? '+' : ''}{entry.pnl.toFixed(2)}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}
