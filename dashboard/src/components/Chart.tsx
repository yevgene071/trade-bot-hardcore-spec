import React, { useEffect, useRef } from 'react';
import { createChart, ColorType, ISeriesApi, CandlestickSeries } from 'lightweight-charts';
import { useTradeStore } from '../store/useTradeStore';

export function CandlestickChart({ ticker }: { ticker: string }) {
  const chartContainerRef = useRef<HTMLDivElement>(null);
  const currentPrice = useTradeStore(state => state.tickerPrices[ticker]) || 64200;
  const chartRef = useRef<any>(null);
  const seriesRef = useRef<ISeriesApi<"Candlestick"> | null>(null);

  useEffect(() => {
    if (!chartContainerRef.current) return;

    const chart = createChart(chartContainerRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: 'transparent' },
        textColor: '#6b7280',
      },
      grid: {
        vertLines: { color: 'rgba(255, 255, 255, 0.05)' },
        horzLines: { color: 'rgba(255, 255, 255, 0.05)' },
      },
      width: chartContainerRef.current.clientWidth || 600,
      height: chartContainerRef.current.clientHeight || 400,
      timeScale: {
        timeVisible: true,
        secondsVisible: false,
      },
      rightPriceScale: {
        borderVisible: false,
      }
    });

    const candlestickSeries = chart.addSeries(CandlestickSeries, {
      upColor: '#10b981',
      downColor: '#f43f5e',
      borderVisible: false,
      wickUpColor: '#10b981',
      wickDownColor: '#f43f5e',
    });

    // Mock initial data
    const data = [];
    let time = Math.floor(Date.now() / 1000) - 100 * 60;
    let price = currentPrice;
    for (let i = 0; i < 100; i++) {
       const open = price;
       const close = price + (Math.random() - 0.5) * 50;
       const high = Math.max(open, close) + Math.random() * 20;
       const low = Math.min(open, close) - Math.random() * 20;
       data.push({ time: time as any, open, high, low, close });
       time += 60;
       price = close;
    }
    candlestickSeries.setData(data);

    chartRef.current = chart;
    seriesRef.current = candlestickSeries;

    const resizeObserver = new ResizeObserver(entries => {
      if (entries.length === 0 || entries[0].target !== chartContainerRef.current) {
        return;
      }
      const newRect = entries[0].contentRect;
      chart.applyOptions({ width: newRect.width, height: newRect.height });
    });

    resizeObserver.observe(chartContainerRef.current);

    return () => {
      resizeObserver.disconnect();
      chart.remove();
    };
  }, []); // Run once on mount

  // Update current candle on price tick
  useEffect(() => {
    if (seriesRef.current) {
      seriesRef.current.update({
        time: Math.floor(Date.now() / 1000) as any,
        open: currentPrice - 5,
        high: currentPrice + 10,
        low: currentPrice - 10,
        close: currentPrice
      });
    }
  }, [currentPrice]);

  return <div ref={chartContainerRef} className="w-full h-full absolute inset-0" />;
}
