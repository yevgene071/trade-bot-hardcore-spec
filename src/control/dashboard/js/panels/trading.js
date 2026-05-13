'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js
// @depends-on: ../charts/canvas.js
// @depends-on: ../charts/price.js

/**
 * Trading Panel renderer.
 * Phase 6: Extract renderTrading.
 */

function renderTrading(data, forceRender) {
  const bids = data.bids_top20 || [];
  const asks = data.asks_top20 || [];
  const obTicker = data.selected_ticker || _selTicker;

  // ── Ticker selector ──
  const tickersEl = $('trade-tickers');
  if (tickersEl) {
    const universe = data.universe || [];
    const tickers = [...new Set(universe.map((u) => u.ticker))].sort();
    if (forceRender || tickersEl.children.length !== tickers.length) {
      tickersEl.replaceChildren();
      if (tickers.length === 0) {
        const btn = el('span', 'ladder-t active', 'BTC_USDT');
        btn.addEventListener('click', () => {
          setSelTicker('BTC_USDT');
          selectTicker('BTC_USDT');
        });
        tickersEl.appendChild(btn);
      } else {
        tickers.forEach((t) => {
          const btn = el('span', 'ladder-t', t);
          btn.addEventListener('click', () => {
            setSelTicker(t);
            setSelTickerTime(Date.now());
            selectTicker(t);
          });
          if (t === obTicker) btn.classList.add('active');
          tickersEl.appendChild(btn);
        });
      }
    } else {
      tickersEl
        .querySelectorAll('.ladder-t')
        .forEach((b) => b.classList.toggle('active', b.textContent === obTicker));
    }
  }

  // ── DOM Ladder hash (cheap — compute first to gate expensive derivations) ──
  const ladderHash = JSON.stringify({
    b: bids.map((x) => [+(x.price || 0).toFixed(2), +(x.size || 0).toFixed(4)]),
    a: asks.map((x) => [+(x.price || 0).toFixed(2), +(x.size || 0).toFixed(4)]),
  });
  const ladderChanged = ladderHash !== _lastTradingHash || forceRender;

  // ── Derive tick / bestAsk / bestBid / sticks ONLY on ladder change ──
  let tick = 0.01,
    dec = 3,
    bestAsk = 0,
    bestBid = 0,
    spreadPrice = 0;
  if (ladderChanged) {
    const candidates = [];
    for (let i = 1; i < asks.length; i++) {
      const d = Math.abs(asks[i].price - asks[i - 1].price);
      if (d > 0 && isFinite(d)) candidates.push(d);
    }
    for (let i = 1; i < bids.length; i++) {
      const d = Math.abs(bids[i - 1].price - bids[i].price);
      if (d > 0 && isFinite(d)) candidates.push(d);
    }
    if (candidates.length > 0) tick = Math.min(...candidates);
    dec = tick >= 1 ? 1 : tick >= 0.1 ? 2 : tick >= 0.01 ? 3 : tick >= 0.001 ? 4 : 5;
    bestAsk = asks.length ? Math.min(...asks.map((a) => a.price)) : data.ob_mid || 0;
    bestBid = bids.length ? Math.max(...bids.map((b) => b.price)) : data.ob_mid || 0;
    spreadPrice = bestAsk - bestBid;
    setLastSticks(tick > 0 ? Math.round(spreadPrice / tick) : 0);
  }

  // ── KPIs (always update — text-only, cheap) ──
  const spreadEl = $('trade-spread-val');
  if (spreadEl) spreadEl.textContent = (data.ob_spread_bps || 0).toFixed(1);
  const sticksEl = $('trade-sticks-val');
  if (sticksEl) sticksEl.textContent = _lastSticks + ' tk';
  const imbEl = $('trade-imb-val');
  if (imbEl) {
    if (!bids.length && !asks.length) {
      imbEl.textContent = '—';
      imbEl.style.color = '';
    } else {
      const imb = data.ob_imbalance || 0;
      imbEl.textContent = (imb >= 0 ? '+' : '') + imb.toFixed(2);
      imbEl.style.color =
        imb > 0.1
          ? 'var(--positive-bright)'
          : imb < -0.1
            ? 'var(--negative-bright)'
            : 'var(--muted)';
    }
  }

  // ── DOM Ladder (only rebuild on order-book change) ──
  const ladder = $('trade-ladder');

  if ((ladderChanged || !ladder.children.length) && (bids.length || asks.length)) {
    setLastTradingHash(ladderHash);
    const ROW_H = 18;
    const maxSz = Math.max(...bids.map((b) => b.size), ...asks.map((a) => a.size), 0.001);
    const BIG = maxSz * 0.52;
    let html = '';

    // Asks — furthest first (top), best-ask last
    const asksRev = [...asks].reverse();
    asksRev.forEach((a) => {
      const pct = Math.min(100, (a.size / maxSz) * 100).toFixed(0);
      const big = a.size >= BIG;
      html += `<div class="lvl ask-lvl${big ? ' big-ask' : ''}" style="height:${ROW_H}px">
        <div class="a-side"><div class="a-fill" style="width:${pct}%"></div><span class="a-lbl">${fmtSz(a.size)}</span></div>
        <div class="p-cell">${a.price.toFixed(dec)}</div>
        <div></div></div>`;
    });

    // Spread rows — empty price tiers between best-ask and best-bid
    const spreadRows = _lastSticks > 1 ? _lastSticks - 1 : 0;
    for (let i = 0; i < spreadRows; i++) {
      const sp = +(bestAsk - (i + 1) * tick).toFixed(dec);
      html += `<div class="lvl spread-row" style="height:${ROW_H}px">
        <div></div>
        <div class="p-cell">${sp.toFixed(dec)}</div>
        <div></div></div>`;
    }

    // Bids — best-bid first
    bids.forEach((b) => {
      const pct = Math.min(100, (b.size / maxSz) * 100).toFixed(0);
      const big = b.size >= BIG;
      html += `<div class="lvl bid-lvl${big ? ' big-bid' : ''}" style="height:${ROW_H}px">
        <div></div>
        <div class="p-cell">${b.price.toFixed(dec)}</div>
        <div class="b-side"><div class="b-fill" style="width:${pct}%"></div><span class="b-lbl">${fmtSz(b.size)}</span></div></div>`;
    });

    ladder.innerHTML = html;

    // ── Mid-line positioning ──
    if (bestAsk !== bestBid && tick > 0) {
      const askCount = asks.length;
      const spCount = spreadRows;
      const spreadZoneTopY = askCount * ROW_H;
      const spreadZoneBotY = (askCount + spCount) * ROW_H;
      const midFrac = Math.max(
        0,
        Math.min(1, (bestAsk - (data.ob_mid || 0)) / (bestAsk - bestBid))
      );
      const midY = spreadZoneTopY + midFrac * (spreadZoneBotY + ROW_H - spreadZoneTopY);

      const midLine = $('trade-mid-line');
      const midTag = $('trade-mid-tag');
      if (midLine) midLine.style.top = midY + 'px';
      if (midTag) {
        midTag.style.top = midY + 'px';
        midTag.textContent = (data.ob_mid || 0).toFixed(dec);
      }

      // Auto-scroll to mid only on first render (when ladder changed)
      const wrap = $('trade-ladder-wrap');
      if (wrap && (forceRender || ladderChanged)) {
        wrap.scrollTop = Math.max(0, midY - wrap.clientHeight / 2);
      }
    }
  }

  // Empty state — show placeholder
  if (!bids.length && !asks.length && ladder) {
    setLastTradingHash('');
    ladder.innerHTML =
      '<div class="empty-state">No order book data — ticker may not be subscribed</div>';
    const midLine = $('trade-mid-line');
    const midTag = $('trade-mid-tag');
    if (midLine) midLine.style.top = '0px';
    if (midTag) {
      midTag.style.top = '0px';
      midTag.textContent = '';
    }
  }

  // ── Chart ──
  const canvas = $('trade-chart');
  if (canvas) {
    const { ctx, w, h } = setupCanvas(canvas);
    const history = data.chart_history || [];
    if (history.length > 0) {
      const sliced = _chartZoom
        ? history.slice(_chartZoom.start, _chartZoom.end + 1)
        : history.slice(-_tfPoints);
      drawPriceChart(ctx, w, h, sliced);
    }
  }

  // ── Mini charts ──
  const miniSpread = $('trade-mini-spread');
  if (miniSpread) {
    const { ctx, w, h } = setupCanvas(miniSpread);
    const history = data.chart_history || [];
    drawMiniChart(ctx, w, h, history, 'spread_bps', '#e8edf5', 'Spread');
  }
  const miniVol = $('trade-mini-vol');
  if (miniVol) {
    const { ctx, w, h } = setupCanvas(miniVol);
    const history = data.chart_history || [];
    drawMiniChart(ctx, w, h, history, 'buy_vol_5s', 'rgba(16,185,129,0.8)', 'Buy Vol');
  }
  const miniAgg = $('trade-mini-agg');
  if (miniAgg) {
    const { ctx, w, h } = setupCanvas(miniAgg);
    const history = data.chart_history || [];
    drawMiniChart(ctx, w, h, history, 'tape_aggression', 'var(--accent2)', 'Aggression');
  }
  const miniCorr = $('trade-mini-corr');
  if (miniCorr) {
    const { ctx, w, h } = setupCanvas(miniCorr);
    const history = data.chart_history || [];
    drawMiniChart(ctx, w, h, history, 'leader_correlation', 'var(--warning)', 'Correlation');
  }
}
