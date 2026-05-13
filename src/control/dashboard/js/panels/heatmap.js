'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Heatmap and Detail Panel renderers.
 * Phase 6: Extract renderHeatmap + renderDetail.
 */

function renderHeatmap(data) {
  const container = $('heatmap-body');
  if (!container) return;
  const items = data.universe || [];
  const stMap = new Map();
  (data.strategy_states || []).forEach((s) => {
    stMap.set(s.ticker, s);
  });

  // Filter
  const filterRow = $('hm-filter-row');
  if (filterRow) {
    filterRow.replaceChildren();
    ['ALL', 'Ready', 'Trading'].forEach((f) => {
      const chip = el('span', 'filter-chip', f);
      if (_hmFilter === f) chip.classList.add('active');
      chip.addEventListener('click', () => {
        setHmFilter(f);
        renderHeatmap(data);
      });
      filterRow.appendChild(chip);
    });
  }

  const filtered = items.filter((it) => {
    const st = stMap.get(it.ticker);
    if (_hmFilter === 'Ready') return st && st.ready_state === 2;
    if (_hmFilter === 'Trading') return st && st.ready_state === 4;
    return true;
  });

  // Hash diffing
  const hash = filtered.map((it) => it.ticker + it.boosted + (it.strategies || []).join(',')).join('|');
  if (container.dataset.lastHash === hash) {
    // Fast path: update mark prices and scores
    const cards = container.querySelectorAll('.hm-card');
    filtered.forEach((it, i) => {
      if (i >= cards.length) return;
      const card = cards[i];
      const st = stMap.get(it.ticker);
      const score = st ? st.readiness_pct || 0 : 0;
      const scoreColor =
        score > 80 ? 'var(--positive-bright)' : score > 50 ? 'var(--warning)' : 'var(--muted)';
      
      const scoreEl = card.querySelector('.hm-score');
      if (scoreEl) {
        scoreEl.textContent = score.toFixed(0);
        scoreEl.style.color = scoreColor;
      }
      
      const statsEl = card.querySelector('.hm-stats');
      if (statsEl && statsEl.children.length > 0) {
        statsEl.children[0].textContent = fmtT2(it.mark_price);
      }
    });
    return;
  }
  container.dataset.lastHash = hash;

  container.replaceChildren();
  if (!filtered.length) {
    container.appendChild(el('div', 'empty-state', 'No tickers match filter'));
    return;
  }

  filtered.forEach((it) => {
    const card = el('div', 'hm-card');
    card.addEventListener('click', () => selectTicker(it.ticker));
    if (it.ticker === _selTicker) card.classList.add('active');

    const st = stMap.get(it.ticker);
    const score = st ? st.readiness_pct || 0 : 0;
    const scoreColor =
      score > 80 ? 'var(--positive-bright)' : score > 50 ? 'var(--warning)' : 'var(--muted)';
    card.innerHTML = `
      <div class="hm-ticker">${it.ticker}${it.boosted ? '<span class="boosted-star">&#9733;</span>' : ''}</div>
      <div class="hm-score" style="color:${scoreColor}">${score.toFixed(0)}</div>
      <div class="hm-stats">
        <span>${fmtT2(it.mark_price)}</span>
        <span>${(it.strategies || []).join(', ')}</span>
      </div>
    `;
    container.appendChild(card);
  });
}

function renderDetail(data) {
  const t = data.selected_ticker || _selTicker || '—';
  const tickerEl = $('detail-ticker');
  if (tickerEl) tickerEl.textContent = t;

  const priceEl = $('detail-price');
  if (priceEl) priceEl.textContent = fmt$(data.ob_mid || 0);

  const changeEl = $('detail-change');
  if (changeEl) {
    const change = 0; // not available from server currently
    changeEl.textContent = fmtPct(change);
    changeEl.className = 'val ' + (change >= 0 ? 'val-up' : 'val-dn');
  }

  const stats = $('detail-stats');
  if (stats) {
    stats.replaceChildren();
    const addStat = (label, val) => {
      const row = el('div', 'stat-row');
      row.innerHTML = `<span>${label}</span><span class="mono">${val}</span>`;
      stats.appendChild(row);
    };
    addStat('Mid', fmtT(data.ob_mid || 0));
    addStat('Spread bps', (data.ob_spread_bps || 0).toFixed(2));
    addStat('Imbalance', (data.ob_imbalance || 0).toFixed(3));
    addStat('Selected', t);
  }
}

function renderConditions(data) {
  const container = $('cond-list');
  if (!container) return;
  // Derive conditions from strategy_states for selected ticker
  const st = (data.strategy_states || []).find((s) => s.ticker === (data.selected_ticker || _selTicker));
  const conds = st ? st.conditions || [] : [];
  container.replaceChildren();

  if (!conds.length) {
    container.appendChild(el('div', 'empty-state', 'No active conditions'));
    return;
  }

  conds.forEach((c) => {
    const item = el('div', 'cond-item');
    const statusCls = c.met ? 'met' : 'pending';
    item.innerHTML = `
      <div class="cond-info">
        <span class="cond-name">${c.name}</span>
        <span class="cond-desc">${c.current} / ${c.target} ${c.unit || ''}</span>
      </div>
      <div class="cond-status ${statusCls}">${c.met ? 'MET' : 'WAIT'}</div>
    `;
    container.appendChild(item);
  });
}
