import React, { memo, useState, useEffect, useRef, useMemo } from 'react';
import { cn } from '../lib/utils';
import { useTradeStore } from '../store/useTradeStore';

interface LadderRowProps {
  price: number;
  size: number;
  type: 'BID' | 'ASK' | 'EMPTY';
  isCurrentPrice?: boolean;
  maxSize: number;
  signals?: { side: string, value: string }[];
  activeOrders?: { id: string, side: 'BUY' | 'SELL', price: number }[];
  onAddOrder?: (price: number, side: 'BUY'|'SELL') => void;
  onRemoveOrder?: (id: string) => void;
}

const LadderRow = memo(({ price, size, type, isCurrentPrice, maxSize, signals = [], activeOrders = [], onAddOrder, onRemoveOrder }: LadderRowProps) => {
  const widthPct = Math.min(100, (size / maxSize) * 100);
  const isKillSwitch = useTradeStore(state => state.killSwitchActive);

  if (type === 'EMPTY') {
    return (
      <div className={cn(
        "flex items-center px-1 relative group transition-colors text-[10px] h-[18px]",
        isCurrentPrice ? "bg-white/10 ring-[1px] ring-inset ring-white/20 z-10 shadow-[0_0_10px_rgba(255,255,255,0.05)]" : "opacity-40 bg-transparent hover:bg-white/[0.02]"
      )}>
         {/* Signals Area */}
         <div className="absolute left-0 top-0 bottom-0 w-[60px] flex items-center justify-end px-1 gap-0.5 pointer-events-none z-20">
            {signals.map((s, i) => (
               <div key={i} className={cn("rounded-full w-4 h-4 flex items-center justify-center text-[7px] font-black text-white shrink-0 shadow-[0_0_8px_rgba(0,0,0,0.5)]", s.side === 'BUY' ? 'bg-[#10b981]' : 'bg-[#f43f5e]')} title={s.value}>
                  {s.value && s.value.replace(/[^0-9.]/g, '').split('.')[0]}
               </div>
            ))}
         </div>
         {/* Size text (empty) */}
         <div className="w-[50px] text-left text-[#7b859c] z-10 tabular-nums pl-1">
         </div>
         {/* Value area */}
         <div className="flex-1 flex justify-start items-center z-10 relative px-1 border-l border-white/5 bg-white/[0.01]">
         </div>
         {/* Price text */}
         <div className="w-[60px] text-center font-medium z-10 tabular-nums text-[#7b859c]">
           {price.toFixed(1)}
         </div>
         
         {/* My Orders / Action */}
         <div className="absolute right-0 flex justify-end z-30 h-full items-center px-1 gap-1">
             {activeOrders.map(o => (
               <div key={o.id} onClick={(e) => { e.stopPropagation(); onRemoveOrder?.(o.id); }} className={cn("px-1.5 py-[2px] rounded text-[8px] uppercase font-black cursor-pointer shadow-[0_0_8px_rgba(0,0,0,0.5)]", o.side === 'BUY' ? "bg-[#10b981] text-white" : "bg-[#f43f5e] text-white")} title="Click to cancel">
                   {o.side === 'BUY' ? 'Lmt Buy' : 'Lmt Sell'}
               </div>
             ))}
             
             {/* Hover place order */}
             <div className="opacity-0 group-hover:opacity-100 flex gap-0.5" style={{ background: 'rgba(3,5,10,0.9)' }}>
                 <button onClick={() => onAddOrder?.(price, 'BUY')} className="h-[14px] w-[14px] bg-[#10b981]/20 hover:bg-[#10b981] text-transparent hover:text-white rounded flex items-center justify-center text-[10px] font-bold transition-colors">+</button>
                 <button onClick={() => onAddOrder?.(price, 'SELL')} className="h-[14px] w-[14px] bg-[#f43f5e]/20 hover:bg-[#f43f5e] text-transparent hover:text-white rounded flex items-center justify-center text-[10px] font-bold transition-colors">-</button>
             </div>
         </div>
      </div>
    );
  }

  return (
    <div className={cn(
      "flex items-center px-1 relative group cursor-pointer transition-colors text-[10px] h-[18px]",
      isCurrentPrice ? "bg-white/10 ring-[1px] ring-inset ring-white/20 shadow-[0_0_10px_rgba(255,255,255,0.05)]" : "hover:bg-white/[0.05]"
    )}>
       {/* Signals Area */}
       <div className="absolute left-0 top-0 bottom-0 w-[60px] flex items-center justify-end px-1 gap-0.5 pointer-events-none z-20 overflow-visible">
          {signals.map((s, i) => (
             <div key={i} className={cn("rounded-full h-4 w-4 flex items-center justify-center text-[7px] font-black text-white shrink-0 shadow-[0_0_8px_rgba(0,0,0,0.5)] transform -translate-x-[50%]", s.side === 'BUY' ? 'bg-[#10b981]' : 'bg-[#f43f5e]')} title={s.value}>
               {s.value && s.value.replace(/[^0-9.]/g, '').split('.')[0]}
             </div>
          ))}
       </div>
       
       {/* Size text */}
       <div className={cn("w-[50px] text-left z-10 tabular-nums font-bold pl-1", type === 'ASK' ? "text-[#f43f5e]" : "text-[#10b981]")}>
         {size >= 1000 ? `${(size / 1000).toFixed(1)}K` : size.toFixed(0)}
       </div>
       
       {/* Max volume bar area */}
       <div className="flex-1 right-0 top-0 bottom-0 relative z-0 border-l border-white/5 bg-white/[0.01]">
          <div 
            className={cn("absolute left-0 top-[1px] bottom-[1px] transition-all duration-300", type === 'ASK' ? "bg-[#f43f5e]/40" : "bg-[#10b981]/40")} 
            style={{ width: `${widthPct}%` }} 
          />
       </div>
       
       {/* Price text */}
       <div className={cn("w-[60px] text-center font-bold z-10 tabular-nums", type === 'ASK' ? "text-[#f43f5e]" : "text-[#10b981]")}>
         {price.toFixed(1)}
       </div>
       
       {/* Hover Action / Orders */}
       <div className="absolute right-0 flex justify-end z-30 h-full items-center px-1 gap-1">
          {activeOrders.map(o => (
             <div key={o.id} onClick={(e) => { e.stopPropagation(); onRemoveOrder?.(o.id); }} className={cn("px-1.5 py-[2px] rounded text-[8px] uppercase font-black cursor-pointer shadow-[0_0_8px_rgba(0,0,0,0.5)]", o.side === 'BUY' ? "bg-[#10b981] text-white" : "bg-[#f43f5e] text-white")} title="Click to cancel">
               {o.side === 'BUY' ? 'Lmt Buy' : 'Lmt Sell'}
             </div>
          ))}
          
         <div className="opacity-0 group-hover:opacity-100 flex gap-0.5" style={{ background: 'rgba(3,5,10,0.9)' }}>
           <button disabled={isKillSwitch} onClick={() => onAddOrder?.(price, 'BUY')} className={cn(
               "px-1.5 py-[2px] rounded text-[8px] uppercase font-black tracking-widest disabled:opacity-30 disabled:cursor-not-allowed transition-colors",
               "bg-[#10b981]/20 text-[#10b981] hover:bg-[#10b981] hover:text-white"
           )}>
             Buy
           </button>
           <button disabled={isKillSwitch} onClick={() => onAddOrder?.(price, 'SELL')} className={cn(
               "px-1.5 py-[2px] rounded text-[8px] uppercase font-black tracking-widest disabled:opacity-30 disabled:cursor-not-allowed transition-colors",
               "bg-[#f43f5e]/20 text-[#f43f5e] hover:bg-[#f43f5e] hover:text-white"
           )}>
             Sell
           </button>
         </div>
       </div>
    </div>
  );
}, (prev, next) => {
  if (prev.price !== next.price) return false;
  if (prev.type !== next.type) return false;
  if (prev.isCurrentPrice !== next.isCurrentPrice) return false;
  if (prev.signals?.length !== next.signals?.length) return false;
  if (prev.activeOrders?.length !== next.activeOrders?.length) return false;
  const sizeDiff = Math.abs(prev.size - next.size) / (prev.size || 1);
  if (sizeDiff > 0.05) return false;
  return true;
});

export function Ladder({ ticker }: { ticker: string }) {
  const isKillSwitch = useTradeStore(state => state.killSwitchActive);
  const currentPrice = useTradeStore(state => state.tickerPrices[ticker]) || 64200;
  const allSignals = useTradeStore(state => state.signals);
  const activeOrdersStore = useTradeStore(state => state.activeOrders);
  const addOrder = useTradeStore(state => state.addOrder);
  const removeOrder = useTradeStore(state => state.removeOrder);
  const myOrders = useMemo(() => activeOrdersStore.filter(o => o.ticker === ticker), [activeOrdersStore, ticker]);
  
  const recentSignals = useMemo(() => allSignals.filter(s => s.ticker === ticker && Date.now() - s.timestamp < 10000), [allSignals, ticker]);
  
  const orderbook = useTradeStore(state => state.orderbook);

  const [anchorPrice, setAnchorPrice] = useState(currentPrice);
  const [spreadTicks, setSpreadTicks] = useState(1);
  const imbalance = orderbook.imbalance;

  const containerRef = useRef<HTMLDivElement>(null);
  const tickSize = 0.5;
  const nearestAnchorTick = Math.round(anchorPrice / tickSize) * tickSize;
  const currentTick = Math.floor(currentPrice / tickSize) * tickSize;

  useEffect(() => {
     if (Math.abs(currentPrice - anchorPrice) > tickSize * 30) {
        setAnchorPrice(currentTick); // Snap the ladder view if price drifts too far
     }
  }, [currentPrice, anchorPrice, tickSize, currentTick]);

  useEffect(() => {
    const int = setInterval(() => {
       setSpreadTicks(Math.floor(Math.random() * 3) + 1);
    }, 800);
    return () => clearInterval(int);
  }, []);
  
  // Calculate spread bounds
  const bestAsk = currentTick + spreadTicks * tickSize;
  const bestBid = currentTick - (spreadTicks - 0.5) * tickSize;

  const { rows, maxSize } = useMemo(() => {
    const computed = [];
    const totalRows = 80;
    let maxS = 0;
    const hasRealData = orderbook.bids.length > 0 || orderbook.asks.length > 0;

    const bidMap = new Map<number, number>();
    const askMap = new Map<number, number>();
    if (hasRealData) {
      for (const b of orderbook.bids) bidMap.set(Math.round(b.price / tickSize) * tickSize, b.size);
      for (const a of orderbook.asks) askMap.set(Math.round(a.price / tickSize) * tickSize, a.size);
    }

    for (let i = totalRows / 2; i >= -totalRows / 2; i--) {
      const price = nearestAnchorTick + i * tickSize;
      let type: 'ASK' | 'BID' | 'EMPTY' = 'EMPTY';
      let size = 0;

      if (price >= bestAsk) type = 'ASK';
      else if (price <= bestBid) type = 'BID';

      if (type !== 'EMPTY') {
        if (hasRealData) {
          size = type === 'ASK' ? (askMap.get(price) || 0) : (bidMap.get(price) || 0);
        } else {
          const isRound = price % 10 === 0;
          const baseSize = Math.max(0.1, Math.abs(Math.sin(price * 123.456)) * 10);
          size = isRound ? baseSize * 5 : baseSize;
        }
        if (size > maxS) maxS = size;
      }

      const rowSignals = recentSignals.filter(s => s.price !== undefined && Math.abs(s.price - price) < tickSize / 2);
      const rowOrders = myOrders.filter(o => Math.abs(o.price - price) < tickSize / 2);

      computed.push({ price, size, type, isCurrentPrice: price === currentTick, signals: rowSignals.map(s => ({ side: s.side, value: s.value })), activeOrders: rowOrders });
    }
    return { rows: computed, maxSize: maxS || 1 };
  }, [nearestAnchorTick, bestAsk, bestBid, currentTick, tickSize, recentSignals, myOrders, orderbook]);

  useEffect(() => {
    // Initial center scroll
    if (containerRef.current) {
       containerRef.current.scrollTop = containerRef.current.scrollHeight / 2 - containerRef.current.clientHeight / 2;
    }
  }, [anchorPrice]);

  return (
    <div className="h-full flex flex-col bg-black/20 border-r border-white/5 relative">
      {isKillSwitch && (
         <div className="absolute inset-0 bg-[#f43f5e]/5 z-50 pointer-events-none flex items-center justify-center border-[2px] border-[#f43f5e] animate-pulse">
         </div>
      )}
      <div className="p-3 border-b border-white/5 flex items-center justify-between bg-black/40 xl:shrink-0 sticky top-0 z-20">
         <div className="text-[10px] font-bold text-white uppercase tracking-wider flex items-center gap-2">
            Order Book
            <span className="bg-white/10 px-1.5 py-0.5 rounded text-[9px] font-mono">{tickSize}</span>
         </div>
         <div className="flex items-center gap-3">
             <div className="text-[9px] font-mono text-[#7b859c]">Spread: {orderbook.spreadBps > 0 ? orderbook.spreadBps.toFixed(1) + ' bps' : (bestAsk - bestBid).toFixed(1)}</div>
             <div className={cn("text-[9px] font-mono px-1.5 py-0.5 rounded border", imbalance > 0 ? "text-[#10b981] bg-[#10b981]/10 border-[#10b981]/20" : "text-[#f43f5e] bg-[#f43f5e]/10 border-[#f43f5e]/20")}>
                Imb: {imbalance > 0 ? '+' : ''}{imbalance.toFixed(2)}
             </div>
         </div>
      </div>
      
      <div className="flex px-4 py-2 border-b border-white/5 text-[9px] font-bold text-[#6b7280] uppercase tracking-wider bg-white/[0.01] shrink-0 sticky top-[41px] z-20 shadow-[0_4px_10px_rgba(0,0,0,0.2)]">
         <div className="w-1/3">Price</div>
         <div className="w-1/3 text-right">Size</div>
         <div className="w-1/3 text-right pr-2">Action</div>
      </div>
      
      <div ref={containerRef} className="flex-1 overflow-y-auto no-scrollbar relative font-mono text-[11px] leading-tight">
         <div className="py-2">
            {rows.map(row => (
              <LadderRow 
                key={row.price} 
                {...row} 
                maxSize={maxSize} 
                onAddOrder={(price, side) => addOrder({ price, side, ticker })}
                onRemoveOrder={(id) => removeOrder(id)}
              />
            ))}
         </div>
      </div>
    </div>
  );
}
