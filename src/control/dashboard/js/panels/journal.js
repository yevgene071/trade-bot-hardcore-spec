'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js
// @depends-on: ../charts/canvas.js

/**
 * Journal panel renderer.
 * Phase 4: Extract renderJournal.
 */

let _seenJournalKeys = new Set();

function renderJournalPnlChart(entries) {
  const canvas = $('jrn-pnl-chart');
  if (!canvas) return;
  const { ctx, w, h } = setupCanvas(canvas);
  if (!ctx) return;
  const sorted = [...entries].sort((a, b) => a.ts_unix_ms - b.ts_unix_ms);
  if (sorted.length < 2) {
    ctx.fillStyle = '#7b859c';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('Not enough trade data', w / 2, h / 2);
    return;
  }
  let cum = 0;
  const vals = sorted.map((e) => { cum += e.pnl_usd; return cum; });
  const pad = { top: 8, right: 40, bottom: 16, left: 10 };
  const pw = w - pad.left - pad.right;
  const ph = h - pad.top - pad.bottom;
  const minV = Math.min(0, ...vals);
  const maxV = Math.max(0, ...vals);
  const range = maxV - minV || 1;
  const toX = (i) => pad.left + (i / (vals.length - 1)) * pw;
  const toY = (v) => pad.top + ph - ((v - minV) / range) * ph;
  drawGrid(ctx, w, h, pad);
  const zeroY = toY(0);
  ctx.beginPath();
  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.lineWidth = 0.5;
  ctx.moveTo(pad.left, zeroY);
  ctx.lineTo(w - pad.right, zeroY);
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(toX(0), toY(vals[0]));
  vals.forEach((v, i) => ctx.lineTo(toX(i), toY(v)));
  const last = vals[vals.length - 1];
  ctx.strokeStyle = last >= 0 ? 'var(--positive-bright)' : 'var(--negative-bright)';
  ctx.lineWidth = 1.5;
  ctx.stroke();
  ctx.fillStyle = '#7b859c';
  ctx.font = '8px Inter, sans-serif';
  ctx.textAlign = 'right';
  ctx.fillText(fmt$(maxV), w - 4, pad.top + 8);
  ctx.fillText(fmt$(minV), w - 4, h - pad.bottom);
}

function renderJournal(data) {
  const body = $('jrn-body');
  if (!body) return;
  const allEntries = (data.recent_journal || []).slice(0, 50);

  // Journal badge — entries not yet seen
  const jrnBadge = $('jrn-tab-badge');
  if (jrnBadge) {
    const newCount = allEntries.filter(
      (e) => !_seenJournalKeys.has(e.ts_unix_ms + e.plan.ticker)
    ).length;
    jrnBadge.textContent = newCount;
    jrnBadge.style.display = newCount > 0 ? 'inline' : 'none';
  }

  // Populate strategy filter
  const stratFilter = $('jrn-strat-filter');
  if (stratFilter) {
    const strategies = [
      ...new Set(allEntries.map((e) => e.plan.strategy_name).filter(Boolean)),
    ].sort();
    const stratKey = strategies.join(',');
    if (stratFilter.dataset.stratKey !== stratKey) {
      stratFilter.dataset.stratKey = stratKey;
      const cur = stratFilter.value;
      stratFilter.replaceChildren();
      const allOpt = document.createElement('option');
      allOpt.value = '';
      allOpt.textContent = 'All strategies';
      stratFilter.appendChild(allOpt);
      strategies.forEach((s) => {
        const opt = document.createElement('option');
        opt.value = s;
        opt.textContent = s;
        stratFilter.appendChild(opt);
      });
      stratFilter.value = strategies.includes(cur) ? cur : '';
    }
  }

  // Hash diffing — include strategy filter so changing it triggers a re-render
  const curStrat = stratFilter ? stratFilter.value : '';
  const hash = curStrat + '|' + allEntries.map((e) => e.ts_unix_ms + e.plan.ticker + e.pnl_usd).join('|');
  if (body.dataset.lastHash === hash) return;
  body.dataset.lastHash = hash;
  const entries = curStrat
    ? allEntries.filter((e) => e.plan.strategy_name.includes(curStrat))
    : allEntries;

  const countEl = $('jrn-count');
  if (countEl) countEl.textContent = entries.length;

  body.replaceChildren();
  if (!entries.length) {
    const tr = el('tr');
    const td = el('td', 'empty-state', 'No journal entries');
    td.colSpan = 7;
    tr.appendChild(td);
    body.appendChild(tr);
    return;
  }

  entries.forEach((e) => {
    const key = e.ts_unix_ms + e.plan.ticker;
    _seenJournalKeys.add(key);
    const tr = el('tr');
    const pnlCls = e.pnl_usd >= 0 ? 'val-up' : 'val-dn';
    tr.innerHTML = `
      <td class="time">${new Date(e.ts_unix_ms).toISOString().slice(11, 19)}</td>
      <td class="mono" style="font-weight:700;">${e.plan.ticker}</td>
      <td class="${e.plan.side === 1 ? 'val-up' : 'val-dn'}">${e.plan.side === 1 ? 'LONG' : 'SHORT'}</td>
      <td>${stratBadge(e.plan.strategy_name)}</td>
      <td class="mono ${pnlCls}">${fmt$(e.pnl_usd)}</td>
      <td class="mono">${fmtT(e.exit_price)}</td>
      <td class="muted" style="font-size:10px;">${e.cause_of_exit || '—'}</td>
    `;
    body.appendChild(tr);
  });

  renderJournalPnlChart(allEntries);
}
