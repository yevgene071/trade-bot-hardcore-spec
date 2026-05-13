'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Heatmap and Detail Panel renderers.
 * Phase 6: Extract renderHeatmap + renderDetail.
 */

function renderHeatmap(data) {
  const container = $('heatmap-grid');
  if (!container) return;
  const items = data.heatmap || [];

  // Filter
  const filterRow = $('hm-filter-row');
  if (filterRow) {
    filterRow.replaceChildren();
    ['ALL', 'Hot', 'Volatile'].forEach((f) => {
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
    if (_hmFilter === 'Hot') return it.score > 70;
    if (_hmFilter === 'Volatile') return it.volatility > 20;
    return true;
  });

  container.replaceChildren();
  if (!filtered.length) {
    container.appendChild(el('div', 'empty-state', 'No tickers match filter'));
    return;
  }

  filtered.forEach((it) => {
    const card = el('div', 'hm-card');
    card.addEventListener('click', () => selectTicker(it.ticker));
    if (it.ticker === _selTicker) card.classList.add('active');

    const scoreColor =
      it.score > 80 ? 'var(--positive-bright)' : it.score > 50 ? 'var(--warning)' : 'var(--muted)';
    card.innerHTML = `
      <div class="hm-ticker">${it.ticker}</div>
      <div class="hm-score" style="color:${scoreColor}">${it.score.toFixed(0)}</div>
      <div class="hm-stats">
        <span>${fmtPct(it.change_24h)}</span>
        <span>Vol: ${it.volatility.toFixed(1)}</span>
      </div>
    `;
    container.appendChild(card);
  });
}

function renderDetail(data) {
  const d = data.detail || {};
  const t = d.ticker || '—';
  const tickerEl = $('detail-ticker');
  if (tickerEl) tickerEl.textContent = t;

  const priceEl = $('detail-price');
  if (priceEl) priceEl.textContent = fmt$(d.price);

  const changeEl = $('detail-change');
  if (changeEl) {
    changeEl.textContent = fmtPct(d.change_24h);
    changeEl.className = 'val ' + (d.change_24h >= 0 ? 'val-up' : 'val-dn');
  }

  const stats = $('detail-stats');
  if (stats) {
    stats.replaceChildren();
    const addStat = (label, val) => {
      const row = el('div', 'stat-row');
      row.innerHTML = `<span>${label}</span><span class="mono">${val}</span>`;
      stats.appendChild(row);
    };
    addStat('24h High', fmt$(d.high_24h));
    addStat('24h Low', fmt$(d.low_24h));
    addStat('Volume', fmtSz(d.volume_24h));
    addStat('ATR (1h)', fmtT(d.atr_1h));
  }
}

function renderConditions(data) {
  const container = $('cond-list');
  if (!container) return;
  const conds = data.conditions || [];
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
        <span class="cond-desc">${c.description}</span>
      </div>
      <div class="cond-status ${statusCls}">${c.met ? 'MET' : 'WAIT'}</div>
    `;
    container.appendChild(item);
  });
}
