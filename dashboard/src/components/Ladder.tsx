import React, { memo, useState, useEffect, useRef, useMemo } from 'react';
import { cn } from '../lib/utils';
import { useTradeStore } from '../store/useTradeStore';
import { normalizeTicker } from '../utils/tickerUtils';

interface LadderRowProps {
  price: number;
  size: number;
  type: 'BID' | 'ASK' | 'EMPTY';
  isCurrentPrice?: boolean;
  maxSize: number;
  pricePrecision: number;
  signals?: { side: string, value: string }[];
  activeOrders?: { id: string, side: 'BUY' | 'SELL', price: number }[];
  onAddOrder?: (price: number, side: 'BUY'|'SELL') => void;
  onRemoveOrder?: (id: string) => void;
}

const LadderRow = memo(({ price, size, type, isCurrentPrice, maxSize, pricePrecision, signals = [], activeOrders = [], onAddOrder, onRemoveOrder }: LadderRowProps) => {
  const widthPct = Math.min(100, (size / maxSize) * 100);
  const isKillSwitch = useTradeStore(state => state.killSwitchActive);

  if (type === 'EMPTY') {
    return (
      <div className={cn("grid grid-cols-[50px_1fr_42px] px-2.5 py-[1.5px] relative group cursor-default items-center h-[18px]", isCurrentPrice ? "bg-white/5" : "hover:bg-[#0d1422]")}>
        <span className="font-mono text-[9px] text-[#3c4e62] tabular-nums">&nbsp;</span>
        <span className="font-mono text-[9px] text-right text-[#3c4e62] tabular-nums">{price.toFixed(pricePrecision)}</span>
        <span className="font-mono text-[8px] text-right text-[#3c4e62] tabular-nums">&nbsp;</span>
      </div>
    );
  }

  const isWall = widthPct > 80;

  return (
    <div className={cn(
      "grid grid-cols-[50px_1fr_42px] px-2.5 py-[1.5px] relative group cursor-default items-center",
      type === 'ASK' ? (isWall ? "bg-[#100606]" : "bg-[#0a0404]") : (isWall ? "bg-[#03100a]" : "bg-[#030a05]"),
      isCurrentPrice ? "bg-white/5" : "hover:bg-[#0d1422]"
    )}>
       {/* Volume bar */}
       <div
         className="absolute right-0 top-0 bottom-0 opacity-15 z-0"
         style={{
           width: `${widthPct}%`,
           backgroundColor: type === 'ASK' ? '#ae2c2c' : '#1a9448',
         }}
       />

       {/* Size */}
       <span className={cn("font-mono text-[9px] tabular-nums z-10", isWall ? (type === 'ASK' ? "text-[#ae2c2c] font-bold" : "text-[#1a9448] font-bold") : "text-[#3c4e62]")}>
         {size >= 1000 ? `${(size / 1000).toFixed(1)}K` : size.toFixed(size < 1 ? 3 : 1)}
       </span>

       {/* Price */}
       <span className={cn("font-mono text-[9px] text-right font-medium tabular-nums z-10", type === 'ASK' ? "text-[#ae2c2c]" : "text-[#1a9448]", isWall && "font-bold text-[11px]")}>
         {price.toFixed(pricePrecision)}
       </span>

       {/* Depth / Actions */}
       <span className="font-mono text-[8px] text-right tabular-nums z-10 relative group">
         {/* Active orders */}
         {activeOrders.length > 0 && (
           <div className="absolute right-0 top-1/2 -translate-y-1/2 flex gap-1 opacity-100 group-hover:opacity-0 transition-opacity">
             {activeOrders.map(o => (
               <div key={o.id} onClick={(e) => { e.stopPropagation(); onRemoveOrder?.(o.id); }} className={cn("px-1 py-px rounded text-[8px] uppercase font-bold cursor-pointer", o.side === 'BUY' ? "bg-[#1a9448] text-white" : "bg-[#ae2c2c] text-white")} title="Click to cancel">
                 {o.side === 'BUY' ? 'Buy' : 'Sell'}
               </div>
             ))}
           </div>
         )}
         {/* Depth indicator */}
         {activeOrders.length === 0 && size > 0 && (
           <span className={cn("text-[8px]", type === 'ASK' ? "text-[#ae2c2c]/50" : "text-[#1a9448]/50")}>
             {widthPct > 1 ? widthPct.toFixed(0) + '%' : ''}
           </span>
         )}
         {/* Add order hover */}
         <div className="absolute right-0 top-1/2 -translate-y-1/2 opacity-0 group-hover:opacity-100 flex gap-0.5 bg-[rgba(7,11,19,0.9)] px-0.5 py-px">
           <button disabled={isKillSwitch} onClick={() => onAddOrder?.(price, 'BUY')} className="h-5 w-5 bg-[#1a9448]/20 hover:bg-[#1a9448] text-[#3c4e62] hover:text-white rounded flex items-center justify-center text-[8px] font-bold transition-colors">+</button>
           <button disabled={isKillSwitch} onClick={() => onAddOrder?.(price, 'SELL')} className="h-5 w-5 bg-[#ae2c2c]/20 hover:bg-[#ae2c2c] text-[#3c4e62] hover:text-white rounded flex items-center justify-center text-[8px] font-bold transition-colors">−</button>
         </div>
       </span>

       {/* Signal markers */}
       {signals.length > 0 && (
         <div className="absolute left-0 top-0 bottom-0 flex items-center gap-0.5 px-0.5 pointer-events-none z-20">
           {signals.map((s, i) => (
             <div key={i} className={cn("rounded-full w-2.5 h-2.5", s.side === 'BUY' || s.side === 'LONG' ? 'bg-[#1a9448]' : 'bg-[#ae2c2c]')} title={s.value} />
           ))}
         </div>
       )}
    </div>
  );
}, (prev, next) => {
  // Honest comparator: only skip re-render when nothing changed.
  // Previously suppressed size changes <5% — that intentionally hid
  // micro-movements in liquidity from the operator, which is deceptive
  // for a hard-realtime trading dashboard.
  if (prev.price !== next.price) return false;
  if (prev.size !== next.size) return false;
  if (prev.type !== next.type) return false;
  if (prev.isCurrentPrice !== next.isCurrentPrice) return false;
  if (prev.signals?.length !== next.signals?.length) return false;
  if (prev.activeOrders?.length !== next.activeOrders?.length) return false;
  return true;
});

export function Ladder({ ticker }: { ticker: string }) {
  const isKillSwitch = useTradeStore(state => state.killSwitchActive);
  const currentPrice = useTradeStore(state => state.tickerPrices[ticker]);
  const allSignals = useTradeStore(state => state.signals);
  const activeOrdersStore = useTradeStore(state => state.activeOrders);
  const addOrder = useTradeStore(state => state.addOrder);
  const removeOrder = useTradeStore(state => state.removeOrder);
  const myOrders = useMemo(() => {
    const normTicker = normalizeTicker(ticker);
    return activeOrdersStore.filter(o => normalizeTicker(o.ticker) === normTicker);
  }, [activeOrdersStore, ticker]);
  
  const recentSignals = useMemo(() => {
    const cutoff = Date.now() - 10000;
    const normTicker = normalizeTicker(ticker);
    return allSignals.filter(s => normalizeTicker(s.ticker) === normTicker && s.timestamp > cutoff);
  }, [allSignals, ticker]);
  
  const orderbook = useTradeStore(state => state.orderbook);

  // E1-FIX: Fall back to orderbook.mid when currentPrice is 0/undefined
  const [anchorPrice, setAnchorPrice] = useState(currentPrice || orderbook.mid || 0);
  const imbalance = orderbook.imbalance;

  const containerRef = useRef<HTMLDivElement>(null);

  // Real instrument tick = the smallest positive gap between adjacent book
  // prices. Inferring it from live levels keeps the ladder grid aligned with
  // the actual exchange grid for ANY coin; the magnitude heuristic is only a
  // fallback for when the book hasn't arrived yet (otherwise a too-coarse
  // hardcoded tick collapses dozens of real levels into 2-3 rows).
  const inferredTick = useMemo(() => {
    const prices = [...orderbook.bids, ...orderbook.asks]
      .map(l => l.price).filter(p => p > 0).sort((a, b) => a - b);
    let best = Infinity;
    for (let i = 1; i < prices.length; i++) {
      const d = prices[i] - prices[i - 1];
      if (d > 1e-12 && d < best) best = d;
    }
    return Number.isFinite(best) ? best : 0;
  }, [orderbook]);

  const tickSize = inferredTick > 0 ? inferredTick : (() => {
    const p = currentPrice || anchorPrice || 0;
    if (p === 0)    return 0.5;
    if (p < 0.0001) return 0.0000001; // PEPE/SHIB/etc
    if (p < 0.001)  return 0.000001;
    if (p < 0.01)   return 0.00001;
    if (p < 0.1)    return 0.0001;
    if (p < 2)      return 0.001;
    if (p < 50)     return 0.01;
    if (p < 1000)   return 0.1;
    return 0.5;
  })();
  const pricePrecision = Math.max(0, Math.ceil(-Math.log10(tickSize)));
  const nearestAnchorTick = Math.round(anchorPrice / tickSize) * tickSize;
  const currentTick = currentPrice ? Math.floor(currentPrice / tickSize) * tickSize : nearestAnchorTick;

  // Recenter the grid immediately on coin switch
  useEffect(() => { 
    if (currentPrice && currentPrice > 0) {
      setAnchorPrice(currentPrice); 
    }
  }, [ticker]); // eslint-disable-line react-hooks/exhaustive-deps

  // E1-FIX: Also recenter when orderbook.mid arrives and anchor is still 0
  useEffect(() => {
    if (anchorPrice === 0 && orderbook.mid > 0) {
      setAnchorPrice(orderbook.mid);
    }
  }, [anchorPrice, orderbook.mid]);

  useEffect(() => {
     if (currentPrice && Math.abs(currentPrice - anchorPrice) > tickSize * 30) {
        setAnchorPrice(currentTick);
     } else if (!anchorPrice && currentPrice) {
        setAnchorPrice(currentTick);
     }
  }, [currentPrice, anchorPrice, tickSize, currentTick]);

  const bestBid = orderbook.bids.length > 0
    ? Math.max(...orderbook.bids.map(b => b.price))
    : (currentTick > 0 ? currentTick - tickSize : 0);
  const bestAsk = orderbook.asks.length > 0
    ? Math.min(...orderbook.asks.map(a => a.price))
    : (bestBid > 0 ? bestBid + tickSize * 2 : tickSize * 2);

  const { rows, maxSize } = useMemo(() => {
    const computed = [];
    const totalRows = 80;
    let maxS = 0;

    // E6-FIX: Use integer tick indices as Map keys to avoid floating-point
    // equality failures. Math.round(price / tickSize) is an integer, so
    // Map.get() === comparison works reliably.
    const bidMap = new Map<number, number>();
    const askMap = new Map<number, number>();
    for (const b of orderbook.bids) bidMap.set(Math.round(b.price / tickSize), b.size);
    for (const a of orderbook.asks) askMap.set(Math.round(a.price / tickSize), a.size);

    for (let i = totalRows / 2; i >= -totalRows / 2; i--) {
      const price = nearestAnchorTick + i * tickSize;
      const tickIdx = Math.round(price / tickSize); // E6: integer key for lookup
      let type: 'ASK' | 'BID' | 'EMPTY' = 'EMPTY';
      let size = 0;

      if (price >= bestAsk) type = 'ASK';
      else if (price <= bestBid) type = 'BID';

      if (type !== 'EMPTY') {
        size = type === 'ASK' ? (askMap.get(tickIdx) || 0) : (bidMap.get(tickIdx) || 0);
        if (size > 0) {
          if (size > maxS) maxS = size;
        } else {
          type = 'EMPTY'; // no liquidity at this tick — render dimmed, not "0.000"
        }
      }

      const rowSignals = recentSignals.filter(s => s.price !== undefined && Math.abs(s.price - price) < tickSize / 2);
      const rowOrders = myOrders.filter(o => Math.abs(o.price - price) < tickSize / 2);

      computed.push({ price, tickIdx, size, type, isCurrentPrice: currentTick > 0 && Math.abs(price - currentTick) < tickSize / 2, signals: rowSignals.map(s => ({ side: s.side, value: s.value })), activeOrders: rowOrders });
    }
    return { rows: computed, maxSize: maxS || 1 };
  }, [nearestAnchorTick, bestAsk, bestBid, currentTick, tickSize, recentSignals, myOrders, orderbook]);

  useEffect(() => {
    if (containerRef.current) {
       containerRef.current.scrollTop = containerRef.current.scrollHeight / 2 - containerRef.current.clientHeight / 2;
    }
  }, [anchorPrice]);

  return (
    <div className="h-full flex flex-col bg-[#070b13] relative">
      {isKillSwitch && (
         <div className="absolute inset-0 bg-[#ae2c2c]/5 z-50 pointer-events-none flex items-center justify-center border-2 border-[#ae2c2c] animate-pulse" />
      )}
      {/* Header */}
      <div className="h-7 bg-[#0a0f1a] border-b border-[#141e30] flex items-center justify-between px-2.5 shrink-0">
         <div className="flex items-center gap-2">
           <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.12em]">Order Book</span>
           <span className="font-mono text-[9px] text-[#c08828] font-semibold tracking-[.04em]">{ticker.split(':')[1]?.replace('.p', '') ?? ticker}</span>
         </div>
      </div>
      {/* Column headers */}
      <div className="grid grid-cols-[50px_1fr_42px] px-2.5 py-[3px] bg-[#0a0f1a] border-b border-[#0c1220] shrink-0">
        <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Size</span>
        <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em] text-right">Price</span>
        <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em] text-right">Depth</span>
      </div>
      {/* Rows */}
      <div ref={containerRef} className="flex-1 overflow-y-auto no-scrollbar">
        {/* Asks (fills bottom-up) */}
        <div className="flex flex-col justify-end">
          {rows.filter(r => r.type === 'ASK').map(row => (
            <LadderRow key={row.tickIdx} {...row} maxSize={maxSize} pricePrecision={pricePrecision} onAddOrder={(price, side) => addOrder({ price, side, ticker })} onRemoveOrder={(id) => removeOrder(id)} />
          ))}
        </div>
        {/* Mid */}
        <div className="h-[30px] bg-[#04060c] border-y border-[#141e30] border-b-2 border-b-[#c08828]/20 flex items-center justify-between px-2.5 shrink-0">
          <span className="font-mono text-[14px] font-bold text-[#d8e8f8]">{currentPrice?.toFixed(pricePrecision) ?? '—'}</span>
          <div className="flex flex-col items-end gap-px">
            <span className="font-mono text-[8px] font-semibold text-[#c08828]">{orderbook.spreadBps > 0 ? orderbook.spreadBps.toFixed(1) + ' bps' : '—'}</span>
            <span className="font-mono text-[8px] text-[#3c4e62] tracking-[.08em]">SPREAD</span>
          </div>
        </div>
        {/* Bids */}
        <div className="flex flex-col">
          {rows.filter(r => r.type === 'BID').map(row => (
            <LadderRow key={row.tickIdx} {...row} maxSize={maxSize} pricePrecision={pricePrecision} onAddOrder={(price, side) => addOrder({ price, side, ticker })} onRemoveOrder={(id) => removeOrder(id)} />
          ))}
        </div>
      </div>
    </div>
  );
}
