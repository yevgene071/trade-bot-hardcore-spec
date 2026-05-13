'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Universe panel renderer.
 * Phase 4: Extract renderUniverse.
 */
function renderUniverse(data) {
  const body = $('uni-body');
  if (!body) return;
  const rows = data.universe || [];
  const states = data.strategy_states || [];
  const stMap = new Map();
  states.forEach((s) => {
    if (!stMap.has(s.ticker)) stMap.set(s.ticker, new Map());
    stMap.get(s.ticker).set(s.strategy_name, s);
  });

  const filterRow = $('uni-filter-row');
  if (filterRow) {
    filterRow.replaceChildren();
    ['ALL', 'Ready', 'Trading'].forEach((f) => {
      const chip = el('span', 'filter-chip', f);
      if (_uniFilter === f) chip.classList.add('active');
      chip.addEventListener('click', () => {
        setUniFilter(f);
        renderUniverse(data);
      });
      filterRow.appendChild(chip);
    });
  }

  body.replaceChildren();
  if (!rows.length) {
    const tr = el('tr');
    const td = el('td', 'empty-state', 'No tickers in universe');
    td.colSpan = 3;
    tr.appendChild(td);
    body.appendChild(tr);
    return;
  }

  rows.forEach((u) => {
    const sm = stMap.get(u.ticker);
    if (_uniFilter === 'Ready' && (!sm || ![...sm.values()].some((s) => s.ready_state === 2)))
      return;
    if (_uniFilter === 'Trading' && (!sm || ![...sm.values()].some((s) => s.ready_state === 4)))
      return;

    const tr = el('tr');
    tr.style.cursor = 'pointer';
    tr.addEventListener('click', () => selectTicker(u.ticker));
    const tdN = el('td');
    tdN.innerHTML = `<span style="font-weight:700;">${u.ticker}</span>${u.boosted ? '<span class="boosted-star">&#9733;</span>' : ''}`;
    tr.appendChild(tdN);
    tr.appendChild(el('td', 'mono', fmtT2(u.mark_price)));
    const tdS = el('td');
    if (sm) {
      tdS.innerHTML =
        (u.strategies || [])
          .map((sn) => {
            const ss = sm.get(sn);
            const cls = stClass(ss ? ss.ready_state : 0);
            return `<span class="state-dot ${cls}" title="${sn}: ${ss ? stName(ss.ready_state) : 'Cold'}"></span>`;
          })
          .join('') +
        ' ' +
        (u.strategies || [])
          .map((sn) => {
            const cls = sn.includes('bounce')
              ? 'strat-bounce'
              : sn.includes('breakout')
                ? 'strat-breakout'
                : sn.includes('leaderlag')
                  ? 'strat-leaderlag'
                  : '';
            return `<span class="strat-badge ${cls}">${sn}</span>`;
          })
          .join('');
    } else {
      tdS.innerHTML = (u.strategies || [])
        .map((sn) => {
          const cls = sn.includes('bounce')
            ? 'strat-bounce'
            : sn.includes('breakout')
              ? 'strat-breakout'
              : sn.includes('leaderlag')
                ? 'strat-leaderlag'
                : '';
          return `<span class="strat-badge ${cls}">${sn}</span>`;
        })
        .join('');
    }
    tr.appendChild(tdS);
    body.appendChild(tr);
  });
}
