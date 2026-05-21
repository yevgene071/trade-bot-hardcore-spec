/**
 * MarketDataService — Zero-allocation market data buffers for HFT dashboard.
 * 
 * PERF-02: Replaces immutable Zustand state with mutable circular buffers.
 * Uses Float64Array for zero GC pressure. Designed for 30 tickers @ 100ms updates.
 * 
 * Architecture:
 * - Each ticker has its own TickerBuffer
 * - Buffers are mutable (in-place updates)
 * - No allocations in hot path (push/update methods)
 * - React components read directly from buffers in RAF
 */

import type { ChartPoint, ObLevel, DensityColumn } from '../types';
import { normalizeTicker } from '../utils/tickerUtils';

/**
 * Mutable circular buffer for chart data.
 * Stores [ts, price, vol, ts, price, vol, ...] in Float64Array.
 */
class ChartBuffer {
  private data: Float64Array;
  private capacity: number;
  private head = 0;
  private size = 0;
  
  // Cached metadata for last point (avoid recomputing in RAF)
  private lastTs = 0;
  private lastMid = 0;
  private lastSpreadBps = 0;
  
  constructor(capacity = 600) {
    this.capacity = capacity;
    // Each point: ts, mid, best_bid, best_ask, spread_bps, buy_vol, sell_vol,
    // volatility, aggression, leader_change, leader_corr, leader_lag, imbalance, prints
    this.data = new Float64Array(capacity * 14);
  }
  
  /**
   * Push new chart point. O(1), zero allocations.
   * IMPORTANT: Assumes points arrive in chronological order (server guarantees this).
   */
  push(point: ChartPoint): void {
    // G1: Ensure chronological order (monotonicity)
    if (this.size > 0 && point.ts_unix_ms <= this.lastTs) {
      // G1: Drop non-monotonic or duplicate points to prevent visual glitches in charts
      return; 
    }

    const idx = this.head * 14;
    
    this.data[idx + 0] = point.ts_unix_ms;
    this.data[idx + 1] = point.mid;
    this.data[idx + 2] = point.best_bid;
    this.data[idx + 3] = point.best_ask;
    this.data[idx + 4] = point.spread_bps;
    this.data[idx + 5] = point.buy_vol_5s;
    this.data[idx + 6] = point.sell_vol_5s;
    this.data[idx + 7] = point.volatility_1min_bps;
    this.data[idx + 8] = point.tape_aggression;
    this.data[idx + 9] = point.leader_change_1s;
    this.data[idx + 10] = point.leader_correlation;
    this.data[idx + 11] = point.leader_lag_ms;
    this.data[idx + 12] = point.imbalance;
    this.data[idx + 13] = point.prints_per_sec;
    
    // Update cached metadata
    this.lastTs = point.ts_unix_ms;
    this.lastMid = point.mid;
    this.lastSpreadBps = point.spread_bps;
    
    this.head = (this.head + 1) % this.capacity;
    this.size = Math.min(this.size + 1, this.capacity);
  }

  clear(): void {
    this.head = 0;
    this.size = 0;
    this.lastTs = 0;
    this.lastMid = 0;
    this.lastSpreadBps = 0;
  }
  
  /**
   * Get raw buffer for direct reading in RAF.
   * Returns reference (not copy) for zero-allocation access.
   */
  getRawData(): Float64Array {
    return this.data;
  }
  
  getSize(): number {
    return this.size;
  }
  
  getHead(): number {
    return this.head;
  }
  
  getCapacity(): number {
    return this.capacity;
  }
  
  /**
   * Get last point metadata without iterating buffer.
   * Used for dirty flag checks in RAF.
   */
  getLastMetadata(): { ts: number; mid: number; spreadBps: number } {
    return {
      ts: this.lastTs,
      mid: this.lastMid,
      spreadBps: this.lastSpreadBps,
    };
  }
  
  /**
   * Extract point at index i (chronological order).
   * Used for rendering. Inline for performance.
   */
  getPoint(i: number): ChartPoint {
    const actualIdx = this.size < this.capacity 
      ? i 
      : (this.head + i) % this.capacity;
    const offset = actualIdx * 14;
    
    return {
      ts_unix_ms: this.data[offset + 0],
      mid: this.data[offset + 1],
      best_bid: this.data[offset + 2],
      best_ask: this.data[offset + 3],
      spread_bps: this.data[offset + 4],
      buy_vol_5s: this.data[offset + 5],
      sell_vol_5s: this.data[offset + 6],
      volatility_1min_bps: this.data[offset + 7],
      tape_aggression: this.data[offset + 8],
      leader_change_1s: this.data[offset + 9],
      leader_correlation: this.data[offset + 10],
      leader_lag_ms: this.data[offset + 11],
      imbalance: this.data[offset + 12],
      prints_per_sec: this.data[offset + 13],
    };
  }
  
  /**
   * Legacy compatibility: return array of points.
   * PERF WARNING: Creates new array. Use getPoint() in RAF instead.
   */
  toArray(): ChartPoint[] {
    const result: ChartPoint[] = [];
    for (let i = 0; i < this.size; i++) {
      result.push(this.getPoint(i));
    }
    return result;
  }

  /**
   * F3a-FIX: Check if buffer has valid mid prices without allocating arrays.
   */
  hasValidPrices(): boolean {
    if (this.size === 0) return false;
    return this.lastMid > 0;
  }

  /**
   * F3a-FIX: Detect data gaps without allocating arrays.
   * Returns array of gap descriptions (only when gaps exist).
   */
  getGaps(thresholdMs: number = 30000): { from: number; to: number; durationMs: number }[] {
    const gaps: { from: number; to: number; durationMs: number }[] = [];
    for (let i = 1; i < this.size; i++) {
      const prevTs = this.getPoint(i - 1).ts_unix_ms;
      const curTs = this.getPoint(i).ts_unix_ms;
      const diff = curTs - prevTs;
      if (diff > thresholdMs) {
        gaps.push({ from: prevTs, to: curTs, durationMs: diff });
      }
    }
    return gaps;
  }

  /**
   * F3b-FIX: Find point nearest to a given X coordinate without toArray().
   * Used by crosshair lookup.
   */
  findNearest(targetTs: number): ChartPoint | null {
    if (this.size === 0) return null;
    // Binary search for nearest timestamp
    let lo = 0, hi = this.size - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.getPoint(mid).ts_unix_ms < targetTs) lo = mid + 1;
      else hi = mid;
    }
    // Check lo and lo-1 for closest
    const pLo = this.getPoint(lo);
    if (lo > 0) {
      const pPrev = this.getPoint(lo - 1);
      if (Math.abs(pPrev.ts_unix_ms - targetTs) < Math.abs(pLo.ts_unix_ms - targetTs)) {
        return pPrev;
      }
    }
    return pLo;
  }
}

/**
 * Mutable order book buffer.
 * Stores [price, size, price, size, ...] for bids and asks.
 */
class OrderBookBuffer {
  private bids: Float64Array;
  private asks: Float64Array;
  private bidCount = 0;
  private askCount = 0;
  
  // Cached aggregates
  private mid = 0;
  private spreadBps = 0;
  private imbalance = 0;
  
  constructor(maxLevels = 20) {
    this.bids = new Float64Array(maxLevels * 2);
    this.asks = new Float64Array(maxLevels * 2);
  }
  
  /**
   * Update order book. O(n) where n = levels, zero allocations.
   */
  update(bids: ObLevel[], asks: ObLevel[], mid: number, spreadBps: number, imbalance: number): void {
    // Update bids
    this.bidCount = Math.min(bids.length, this.bids.length / 2);
    for (let i = 0; i < this.bidCount; i++) {
      this.bids[i * 2] = bids[i].price;
      this.bids[i * 2 + 1] = bids[i].size;
    }
    
    // Update asks
    this.askCount = Math.min(asks.length, this.asks.length / 2);
    for (let i = 0; i < this.askCount; i++) {
      this.asks[i * 2] = asks[i].price;
      this.asks[i * 2 + 1] = asks[i].size;
    }
    
    // Cache aggregates
    this.mid = mid;
    this.spreadBps = spreadBps;
    this.imbalance = imbalance;
  }
  
  getBids(): Float64Array {
    return this.bids;
  }
  
  getAsks(): Float64Array {
    return this.asks;
  }
  
  getBidCount(): number {
    return this.bidCount;
  }
  
  getAskCount(): number {
    return this.askCount;
  }
  
  getMid(): number {
    return this.mid;
  }
  
  getSpreadBps(): number {
    return this.spreadBps;
  }
  
  getImbalance(): number {
    return this.imbalance;
  }
  
  /**
   * Legacy compatibility: return array of levels.
   * PERF WARNING: Creates new arrays.
   */
  toArrays(): { bids: ObLevel[]; asks: ObLevel[] } {
    const bids: ObLevel[] = [];
    for (let i = 0; i < this.bidCount; i++) {
      bids.push({
        price: this.bids[i * 2],
        size: this.bids[i * 2 + 1],
      });
    }
    
    const asks: ObLevel[] = [];
    for (let i = 0; i < this.askCount; i++) {
      asks.push({
        price: this.asks[i * 2],
        size: this.asks[i * 2 + 1],
      });
    }
    
    return { bids, asks };
  }
}

/**
 * Mutable density history buffer.
 * Stores density columns (24 bins per column).
 */
class DensityBuffer {
  private columns: DensityColumn[] = [];
  private capacity: number;
  
  constructor(capacity = 300) {
    this.capacity = capacity;
  }
  
  /**
   * Push new density column. O(1) amortized.
   */
  push(column: DensityColumn): void {
    if (this.columns.length < this.capacity) {
      this.columns.push(column);
    } else {
      // G2: Circular shift using head pointer would be better, but for now 
      // just use index-based shift to avoid Array.shift() O(N).
      // Since columns is small (300), this is acceptable but not ideal.
      for (let i = 0; i < this.capacity - 1; i++) {
        this.columns[i] = this.columns[i + 1];
      }
      this.columns[this.capacity - 1] = column;
    }
  }
  
  /**
   * Get all columns (reference, not copy).
   */
  getColumns(): DensityColumn[] {
    return this.columns;
  }
  
  clear(): void {
    this.columns.length = 0;
  }
}

/**
 * Per-ticker buffer container.
 */
class TickerBuffer {
  readonly chart: ChartBuffer;
  readonly orderbook: OrderBookBuffer;
  readonly density: DensityBuffer;
  
  constructor() {
    this.chart = new ChartBuffer(600);
    this.orderbook = new OrderBookBuffer(20);
    this.density = new DensityBuffer(300);
  }
}

/**
 * Global market data service.
 * Singleton pattern for easy access from React components.
 */
class MarketDataService {
  private buffers = new Map<string, TickerBuffer>();
  
  /**
   * Get or create buffer for ticker.
   */
  getBuffer(ticker: string): TickerBuffer {
    const normalized = normalizeTicker(ticker);
    
    let buffer = this.buffers.get(normalized);
    if (!buffer) {
      // G3: Eviction if too many buffers (prevent leak)
      if (this.buffers.size > 50) {
        const oldest = this.buffers.keys().next().value;
        if (oldest) this.buffers.delete(oldest);
      }
      buffer = new TickerBuffer();
      this.buffers.set(normalized, buffer);
    }
    return buffer;
  }
  
  /**
   * Check if ticker has buffer.
   */
  hasBuffer(ticker: string): boolean {
    return this.buffers.has(normalizeTicker(ticker));
  }
  
  /**
   * Clear buffer for ticker (e.g., on ticker switch).
   */
  clearBuffer(ticker: string): void {
    this.buffers.delete(normalizeTicker(ticker));
  }
  
  /**
   * Clear all buffers (e.g., on reconnect).
   */
  clearAll(): void {
    this.buffers.clear();
  }
  
  /**
   * Get all ticker names with buffers.
   */
  getTickers(): string[] {
    return Array.from(this.buffers.keys());
  }
}

// Singleton instance
export const marketDataService = new MarketDataService();

// Export types for use in components
export type { ChartBuffer, OrderBookBuffer, DensityBuffer, TickerBuffer };
