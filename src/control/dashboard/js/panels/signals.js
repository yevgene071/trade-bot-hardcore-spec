'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Signals panel renderer.
 * Phase 4: Extract buildSignalFilters, renderSignals, renderSigCounts.
 */
function buildSignalFilters(data) {
  const container = $('sig-filter-row');
  if (!container) return;
  const sigs = data.signals || [];
  const kinds = ['ALL', ...new Set(sigs.map(s => s.kind))].sort();

  if (_lastSigFilter === JSON.stringify(kinds)) return;
  setSigFilter(JSON.stringify(kinds)); // Use setter for last filter tracking

  container.replaceChildren();
  kinds.forEach(k => {
    const chip = el('span', 'filter-chip', k);
    if (_sigFilter === k || (_sigFilter === '' && k === 'ALL')) chip.classList.add('active');
    chip.addEventListener('click', () => {
      setSigFilter(k === 'ALL' ? '' : k);
      document.querySelectorAll('#sig-filter-row .filter-chip').forEach(c => c.classList.remove('active'));
      chip.classList.add('active');
      renderSignals(data);
    });
    container.appendChild(chip);
  });
}

function renderSignals(data) {
  const sigs = data.signals || [];
  const filtered = _sigFilter ? sigs.filter(s => s.kind === _sigFilter) : sigs;
  const MAX_FEED = 100;

  const badge = $('sig-tab-badge');
  if (badge) {
    const newCount = sigs.filter(s => !_seenSignals.has(s.time_str + s.ticker + s.kind)).length;
    badge.textContent = newCount;
    badge.style.display = newCount ? 'inline' : 'none';
  }

  // ── Incremental feed (main tab) ──
  const feed = $('sig-feed');
  if (feed) {
    let newCount = 0;
    while (newCount < filtered.length) {
      if (_renderedSigKeys.has(filtered[newCount].time_str + filtered[newCount].ticker + filtered[newCount].kind)) break;
      newCount++;
    }

    if (filtered.length === 0) {
      feed.replaceChildren();
    } else if (newCount >= filtered.length || feed.children.length === 0) {
      feed.replaceChildren();
      filtered.slice(0, MAX_FEED).forEach(s => {
        const key = s.time_str + s.ticker + s.kind;
        _renderedSigKeys.add(key);
        const div = el('div', 'feed-item ' + sigClass(s.kind));
        div.dataset.sk = key;
        const isNew = !_seenSignals.has(key);
        div.innerHTML = `<span class="time">[${s.time_str}]</span> <span class="ticker">${s.ticker}</span> <span class="kind">${s.kind}</span> @ <span class="mono">${fmtT(s.price)}</span> ${isNew ? '<span class="sig-new-badge">NEW</span>' : ''}`;
        div.addEventListener('click', () => selectTicker(s.ticker));
        feed.appendChild(div);
      });
    } else if (newCount > 0) {
      const atBottom = feed.scrollHeight - feed.scrollTop <= feed.clientHeight + 40;
      for (let i = newCount - 1; i >= 0; i--) {
        const s = filtered[i];
        const key = s.time_str + s.ticker + s.kind;
        _renderedSigKeys.add(key);
        const div = el('div', 'feed-item signal-new ' + sigClass(s.kind));
        div.dataset.sk = key;
        const isNew = !_seenSignals.has(key);
        div.innerHTML = `<span class="time">[${s.time_str}]</span> <span class="ticker">${s.ticker}</span> <span class="kind">${s.kind}</span> @ <span class="mono">${fmtT(s.price)}</span> ${isNew ? '<span class="sig-new-badge">NEW</span>' : ''}`;
        div.addEventListener('click', () => selectTicker(s.ticker));
        feed.insertBefore(div, feed.firstChild);
      }
      if (atBottom) feed.scrollTop = 0;
    }

    while (feed.children.length > MAX_FEED) {
      feed.removeChild(feed.lastChild);
    }

    _renderedSigKeys.clear();
    for (let i = 0; i < filtered.length; i++) {
      _renderedSigKeys.add(filtered[i].time_str + filtered[i].ticker + filtered[i].kind);
    }
    for (let i = feed.children.length - 1; i >= 0; i--) {
      const sk = feed.children[i].dataset.sk;
      if (sk && !_renderedSigKeys.has(sk)) feed.removeChild(feed.children[i]);
    }
  }

  // ── Full signals tab (grouped by ticker) ──
  const feedFull = $('sig-feed-full');
  if (feedFull) {
    let newCountFull = 0;
    while (newCountFull < filtered.length) {
      if (_renderedSigFullKeys.has(filtered[newCountFull].time_str + filtered[newCountFull].ticker + filtered[newCountFull].kind)) break;
      newCountFull++;
    }

    if (filtered.length === 0) {
      feedFull.replaceChildren();
    } else if (newCountFull >= filtered.length || feedFull.children.length === 0) {
      feedFull.replaceChildren();
      const groups = new Map();
      filtered.forEach(s => {
        if (!groups.has(s.ticker)) groups.set(s.ticker, []);
        groups.get(s.ticker).push(s);
      });
      [...groups.entries()].sort((a, b) => a[0].localeCompare(b[0])).forEach(([ticker, gsigs]) => {
        const grp = el('div', 'sig-group');
        const key = 'sig-grp-' + ticker;
        if (_groupOpen[key] === undefined) _groupOpen[key] = true;
        const head = el('div', 'sig-group-head');
        head.innerHTML = `<span>${ticker} <span style="color:var(--muted)">(${gsigs.length})</span></span><span style="font-size:10px;">${_groupOpen[key] ? '▼' : '▶'}</span>`;
        head.addEventListener('click', () => {
          _groupOpen[key] = !_groupOpen[key];
          const body = grp.querySelector('.sig-group-body');
          if (body) body.classList.toggle('open', _groupOpen[key]);
          head.querySelector('span:last-child').textContent = _groupOpen[key] ? '▼' : '▶';
        });
        grp.appendChild(head);
        const bodyEl = el('div', 'sig-group-body');
        if (_groupOpen[key]) bodyEl.classList.add('open');
        gsigs.forEach(s => {
          const sigKey = s.time_str + s.ticker + s.kind;
          _renderedSigFullKeys.add(sigKey);
          const div = el('div', 'feed-item ' + sigClass(s.kind));
          div.dataset.sk = sigKey;
          const isNew = !_seenSignals.has(sigKey);
          _seenSignals.add(sigKey);
          div.innerHTML = `<span class="time">[${s.time_str}]</span> <span class="kind">${s.kind}</span> @ <span class="mono">${fmtT(s.price)}</span> <span class="muted">(${(s.confidence || 0).toFixed(2)})</span>${isNew ? '<span class="sig-new-badge">NEW</span>' : ''}`;
          bodyEl.appendChild(div);
        });
        grp.appendChild(bodyEl);
        feedFull.appendChild(grp);
      });
    } else if (newCountFull > 0) {
      const atBottom = feedFull.scrollHeight - feedFull.scrollTop <= feedFull.clientHeight + 40;
      const newByTicker = new Map();
      for (let i = 0; i < newCountFull; i++) {
        const s = filtered[i];
        if (!newByTicker.has(s.ticker)) newByTicker.set(s.ticker, []);
        newByTicker.get(s.ticker).push(s);
      }

      for (const [ticker, newSigs] of newByTicker) {
        const grp = Array.from(feedFull.children).find(c => {
          const h = c.querySelector('.sig-group-head');
          return h && h.textContent.trimStart().startsWith(ticker);
        });

        if (grp) {
          const bodyEl = grp.querySelector('.sig-group-body');
          const head = grp.querySelector('.sig-group-head');
          if (bodyEl && head) {
            for (let i = newSigs.length - 1; i >= 0; i--) {
              const s = newSigs[i];
              const key = s.time_str + s.ticker + s.kind;
              _renderedSigFullKeys.add(key);
              const div = el('div', 'feed-item signal-new ' + sigClass(s.kind));
              div.dataset.sk = key;
              const isNew = !_seenSignals.has(key);
              _seenSignals.add(key);
              div.innerHTML = `<span class="time">[${s.time_str}]</span> <span class="kind">${s.kind}</span> @ <span class="mono">${fmtT(s.price)}</span> <span class="muted">(${(s.confidence || 0).toFixed(2)})</span>${isNew ? '<span class="sig-new-badge">NEW</span>' : ''}`;
              bodyEl.insertBefore(div, bodyEl.firstChild);
            }
            const sigSpan = head.querySelector('span:first-child');
            if (sigSpan) {
              sigSpan.innerHTML = `${ticker} <span style="color:var(--muted)">(${bodyEl.children.length})</span>`;
            }
          }
        } else {
          const grp = el('div', 'sig-group');
          const key = 'sig-grp-' + ticker;
          _groupOpen[key] = true;
          const head = el('div', 'sig-group-head');
          head.innerHTML = `<span>${ticker} <span style="color:var(--muted)">(${newSigs.length})</span></span><span style="font-size:10px;">▼</span>`;
          head.addEventListener('click', () => {
            _groupOpen[key] = !_groupOpen[key];
            const body = grp.querySelector('.sig-group-body');
            if (body) body.classList.toggle('open', _groupOpen[key]);
            head.querySelector('span:last-child').textContent = _groupOpen[key] ? '▼' : '▶';
          });
          grp.appendChild(head);
          const bodyEl = el('div', 'sig-group-body');
          bodyEl.classList.add('open');
          for (let i = newSigs.length - 1; i >= 0; i--) {
            const s = newSigs[i];
            const sk = s.time_str + s.ticker + s.kind;
            _renderedSigFullKeys.add(sk);
            const div = el('div', 'feed-item signal-new ' + sigClass(s.kind));
            div.dataset.sk = sk;
            const isNew = !_seenSignals.has(sk);
            _seenSignals.add(sk);
            div.innerHTML = `<span class="time">[${s.time_str}]</span> <span class="kind">${s.kind}</span> @ <span class="mono">${fmtT(s.price)}</span> <span class="muted">(${(s.confidence || 0).toFixed(2)})</span>${isNew ? '<span class="sig-new-badge">NEW</span>' : ''}`;
            bodyEl.appendChild(div);
          }
          grp.appendChild(bodyEl);
          feedFull.insertBefore(grp, feedFull.firstChild);
        }
      }
      if (atBottom) feedFull.scrollTop = feedFull.scrollHeight;
    }

    for (const grp of feedFull.children) {
      const bodyEl = grp.querySelector('.sig-group-body');
      if (bodyEl && bodyEl.children.length > MAX_FEED) {
        while (bodyEl.children.length > MAX_FEED) {
          bodyEl.removeChild(bodyEl.lastChild);
        }
      }
    }
    _renderedSigFullKeys.clear();
    for (let i = 0; i < filtered.length; i++) {
      _renderedSigFullKeys.add(filtered[i].time_str + filtered[i].ticker + filtered[i].kind);
    }
    for (let i = feedFull.children.length - 1; i >= 0; i--) {
      const grp = feedFull.children[i];
      const bodyEl = grp.querySelector('.sig-group-body');
      if (!bodyEl) continue;
      for (let j = bodyEl.children.length - 1; j >= 0; j--) {
        const sk = bodyEl.children[j].dataset.sk;
        if (sk && !_renderedSigFullKeys.has(sk)) bodyEl.removeChild(bodyEl.children[j]);
      }
      if (bodyEl.children.length === 0) feedFull.removeChild(grp);
    }
  }

  if (_seenSignals.size > 10000) {
    const arr = [..._seenSignals];
    _seenSignals = new Set(arr.slice(arr.length - 5000));
  }

  buildSignalFilters(data);
  renderSigCounts(data);
}

function renderSigCounts(data) {
  const body = $('sig-cnt-body');
  if (!body) return;
  const sigs = data.signals || [];
  const counts = {};
  sigs.forEach(s => { counts[s.kind] = (counts[s.kind] || 0) + 1; });

  const sorted = Object.entries(counts).sort((a, b) => b[1] - a[1]);
  body.replaceChildren();
  if (!sorted.length) {
    body.innerHTML = '<div class="empty-state">No signals yet</div>';
    return;
  }

  sorted.forEach(([kind, count]) => {
    const row = el('div', 'sig-cnt-row');
    row.innerHTML = `<span>${kind}</span><span class="mono">${count}</span>`;
    body.appendChild(row);
  });
}
