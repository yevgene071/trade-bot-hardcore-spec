import React, { useState } from 'react';
import { cn } from '../lib/utils';
import { useTradeStore } from '../store/useTradeStore';
import { apiSendCommand } from '../transport/api';

type OrderType = 'limit' | 'market';
type Side = 'BUY' | 'SELL';

export function TradingPanel() {
  const killSwitchActive = useTradeStore(s => s.killSwitchActive);
  const activeTicker = useTradeStore(s => s.activeTicker);

  const [orderType, setOrderType] = useState<OrderType>('limit');
  const [side, setSide] = useState<Side>('BUY');
  const [price, setPrice] = useState('');
  const [size, setSize] = useState('');
  const [status, setStatus] = useState<{ ok: boolean; msg: string } | null>(null);

  const handleSubmit = async () => {
    if (!size || parseFloat(size) <= 0) {
      setStatus({ ok: false, msg: 'Size required' });
      return;
    }
    if (orderType === 'limit' && (!price || parseFloat(price) <= 0)) {
      setStatus({ ok: false, msg: 'Price required for limit' });
      return;
    }
    const cmd = orderType === 'limit'
      ? `order ${side} ${size} ${price} ${activeTicker}`
      : `order ${side} ${size} market ${activeTicker}`;
    try {
      await apiSendCommand(cmd);
      setStatus({ ok: true, msg: 'Order sent' });
      setSize(''); setPrice('');
    } catch (e: any) {
      setStatus({ ok: false, msg: e.message });
    }
  };

  return (
    <div className="bg-black/20 border-t border-white/5 p-4 space-y-3 shrink-0">
      <div className="text-[9px] font-bold text-[#7b859c] uppercase tracking-widest mb-2">Manual Order</div>

      {/* Order type tabs */}
      <div className="flex rounded-lg overflow-hidden border border-white/10">
        {(['limit', 'market'] as OrderType[]).map(t => (
          <button
            key={t}
            onClick={() => setOrderType(t)}
            className={cn(
              "flex-1 py-1.5 text-[10px] font-bold uppercase tracking-wider transition-colors",
              orderType === t ? "bg-white/10 text-white" : "text-[#7b859c] hover:text-white"
            )}
          >{t}</button>
        ))}
      </div>

      {/* Side toggle */}
      <div className="flex gap-2">
        <button
          onClick={() => setSide('BUY')}
          className={cn("flex-1 py-2 rounded-lg text-[10px] font-black uppercase tracking-wider transition-colors border",
            side === 'BUY'
              ? "bg-[#10b981] text-white border-[#10b981]"
              : "bg-[#10b981]/10 text-[#10b981] border-[#10b981]/30 hover:bg-[#10b981]/20"
          )}
        >BUY</button>
        <button
          onClick={() => setSide('SELL')}
          className={cn("flex-1 py-2 rounded-lg text-[10px] font-black uppercase tracking-wider transition-colors border",
            side === 'SELL'
              ? "bg-[#f43f5e] text-white border-[#f43f5e]"
              : "bg-[#f43f5e]/10 text-[#f43f5e] border-[#f43f5e]/30 hover:bg-[#f43f5e]/20"
          )}
        >SELL</button>
      </div>

      {/* Price (only for limit) */}
      {orderType === 'limit' && (
        <input
          type="number"
          value={price}
          onChange={e => setPrice(e.target.value)}
          placeholder="Price"
          className="w-full bg-white/[0.04] border border-white/10 rounded-lg px-3 py-2 text-xs font-mono text-white placeholder-[#7b859c] focus:outline-none focus:border-[#58a6ff]/50"
        />
      )}

      {/* Size */}
      <input
        type="number"
        value={size}
        onChange={e => setSize(e.target.value)}
        placeholder="Size"
        className="w-full bg-white/[0.04] border border-white/10 rounded-lg px-3 py-2 text-xs font-mono text-white placeholder-[#7b859c] focus:outline-none focus:border-[#58a6ff]/50"
      />

      {/* Submit */}
      <button
        onClick={handleSubmit}
        disabled={killSwitchActive}
        className={cn(
          "w-full py-2.5 rounded-lg text-[10px] font-black uppercase tracking-widest transition-all border disabled:opacity-30 disabled:cursor-not-allowed",
          side === 'BUY'
            ? "bg-[#10b981]/15 text-[#10b981] border-[#10b981]/30 hover:bg-[#10b981] hover:text-white"
            : "bg-[#f43f5e]/15 text-[#f43f5e] border-[#f43f5e]/30 hover:bg-[#f43f5e] hover:text-white"
        )}
      >
        {killSwitchActive ? 'KILL SWITCH ACTIVE' : `Place ${side} ${orderType}`}
      </button>

      {status && (
        <div className={cn("text-[10px] font-mono text-center", status.ok ? "text-[#10b981]" : "text-[#f43f5e]")}>
          {status.ok ? '✓' : '✗'} {status.msg}
        </div>
      )}
    </div>
  );
}
