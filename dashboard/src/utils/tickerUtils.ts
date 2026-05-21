/**
 * Normalize ticker from various formats to backend canonical 'BASE_QUOTE':
 * - BINANCE:BTCUSDT.p -> BTC_USDT
 * - BTCUSDT -> BTC_USDT
 * - BTCUSDC -> BTC_USDC
 * - BTC_USDT -> BTC_USDT (passthrough)
 * - XRPUSDT -> XRP_USDT
 */
const QUOTE_ASSETS = ['USDT', 'USDC', 'BUSD', 'BTC', 'ETH', 'BNB'] as const;

export function normalizeTicker(ticker: string): string {
  if (!ticker) return '';
  
  // Strip exchange prefix and suffix: 'BINANCE:BTCUSDT.p' -> 'BTCUSDT'
  let normalized = ticker.includes(':') 
    ? (ticker.split(':')[1].replace('.p', '').replace('.m', ''))
    : ticker;
  
  // Already canonical format
  if (normalized.includes('_')) return normalized;
  
  // Match backend to_ms(): find longest matching quote suffix
  for (const q of QUOTE_ASSETS) {
    if (normalized.length > q.length && normalized.endsWith(q)) {
      return normalized.substring(0, normalized.length - q.length) + '_' + q;
    }
  }
  
  return normalized;
}
