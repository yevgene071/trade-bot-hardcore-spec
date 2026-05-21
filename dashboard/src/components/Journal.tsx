import React from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { cn } from '../lib/utils';
import { format } from 'date-fns';

export function Journal() {
  const journalEntries = useTradeStore(state => state.journalEntries);

  return (
    <div className="h-full flex flex-col bg-[#070b13]">
      <div className="h-7 bg-[#0a0f1a] border-b border-[#141e30] flex items-center px-2.5 shrink-0">
        <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.12em]">
          Trade Journal
        </span>
      </div>
      <div className="flex-1 overflow-auto no-scrollbar">
        <table className="w-full text-left border-collapse text-[11px] font-mono whitespace-nowrap">
          <thead className="sticky top-0 bg-[#0a0f1a] z-10">
            <tr className="text-[#1c2a38] uppercase tracking-wider">
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
            {journalEntries.length === 0 ? (
              <tr>
                <td colSpan={8} className="py-16 text-center text-[#3c4e62] text-xs">
                  No trades executed yet.
                </td>
              </tr>
            ) : (
              journalEntries.map((entry) => {
                const isProfit = entry.pnlUsd >= 0;
                return (
                  <tr key={entry.id} className="border-b border-[#0c1220] hover:bg-white/[0.02] transition-colors">
                    <td className="py-2 px-3 text-[#3c4e62]">
                      {format(entry.exitTimeMs, 'HH:mm:ss')}
                    </td>
                    <td className="py-2 px-3 font-bold text-[#d8e8f8]">
                      {entry.ticker}
                    </td>
                    <td className="py-2 px-3">
                      <span className={cn(
                        "px-1.5 py-0.5 rounded text-[9px] font-black tracking-widest",
                        entry.side === 'LONG' ? "bg-[#1a9448]/20 text-[#1a9448]" : "bg-[#ae2c2c]/20 text-[#ae2c2c]"
                      )}>
                        {entry.side}
                      </span>
                    </td>
                    <td className="py-2 px-3 text-[#d8e8f8]">
                      {entry.size.toFixed(2)}
                    </td>
                    <td className="py-2 px-3 text-[#3c4e62]">
                      {entry.entryPrice.toFixed(1)}
                    </td>
                    <td className="py-2 px-3 text-[#3c4e62]">
                      {entry.exitPrice.toFixed(1)}
                    </td>
                    <td className="py-2 px-3 text-[#3c4e62]">
                      {entry.strategy}
                    </td>
                    <td className={cn(
                      "py-2 px-3 text-right font-bold",
                      isProfit ? "text-[#1a9448] text-glow-green" : "text-[#ae2c2c]"
                    )}>
                      {isProfit ? '+' : ''}{entry.pnlUsd.toFixed(2)}
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
