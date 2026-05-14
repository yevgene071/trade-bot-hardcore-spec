'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Signals panel renderer.
 * Phase 4 + refactored: modular signal rendering with shared templates.
 */

const MAX_FEED = 100;

// ── Key helpers ────────────────────────────────────────────────────────────

function sigKey(s) {
  return s.time_str + s.ticker + s.kind;
}

function countConsecutiveNew(signals, renderedSet) {
  let n = 0;
  while (n < signals.length) {
    if (renderedSet.has(sigKey(signals[n]))) break;
    n++;
  }
  return n;
}

// ── Shared templates ───────────────────────────────────────────────────────

/**
 * Creates a single feed-item DOM element.
 * @param {Object} s - Signal object with time_str, ticker, kind, price, confidence
 * @param {Object} [opts]
 * @param {boolean} [opts.showTicker]     - Include ticker span and click-to-select
 * @param {boolean} [opts.showConfidence] - Show confidence value
 * @param {boolean} [opts.isNew]          - Add NEW badge
 * @param {string}  [opts.extraClass]     - Additional CSS class (e.g. 'signal-new')
 */
function makeFeedItem(s, opts = {}) {
  const { showTicker = true, showConfidence = false, isNew = false, extraClass = '' } = opts;
  const key = sigKey(s);
  let cls = 'feed-item ' + sigClass(s.kind);
  if (extraClass) cls = extraClass + ' ' + cls;
  const div = el('div', cls);
  div.dataset.sk = key;

  let html = '<span class="time">[' + s.time_str + ']</span> ';
  if (showTicker) html += '<span class="ticker">' + s.ticker + '</span> ';
  html +=
    '<span class="kind">' + s.kind + '</span> @ <span class="mono">' + fmtT(s.price) + '</span>';
  if (showConfidence)
    html += ' <span class="muted">(' + (s.confidence || 0).toFixed(2) + ')</span>';
  if (isNew) html += ' <span class="sig-new-badge">NEW</span>';

  div.innerHTML = html;
  if (showTicker) div.addEventListener('click', function () { selectTicker(s.ticker); });
  return div;
}

/**
 * Creates a group header for the full feed.
 * Ensures _groupOpen default for new tickers.
 */
function makeGroupHeader(ticker, count) {
  var key = 'sig-grp-' + ticker;
  if (_groupOpen[key] === undefined) _groupOpen[key] = true;
  var open = _groupOpen[key];

  var head = el('div', 'sig-group-head');
  head.innerHTML =
    '<span>' + ticker + ' <span style="color:var(--muted)">(' + count + ')</span></span>' +
    '<span style="font-size:10px;">' + (open ? '▼' : '▶') + '</span>';
  return head;
}

/**
 * Wires the toggle click on a group header to show/hide its body.
 */
function bindGroupToggle(head, bodyEl, ticker) {
  var key = 'sig-grp-' + ticker;
  head.addEventListener('click', function () {
    _groupOpen[key] = !_groupOpen[key];
    bodyEl.classList.toggle('open', _groupOpen[key]);
    var arrow = head.querySelector('span:last-child');
    if (arrow) arrow.textContent = _groupOpen[key] ? '▼' : '▶';
  });
}

/**
 * Updates the item count displayed inside a group header's first <span>.
 */
function updateGroupCount(head, ticker, count) {
  var firstSpan = head.querySelector('span:first-child');
  if (firstSpan) {
    firstSpan.innerHTML =
      ticker + ' <span style="color:var(--muted)">(' + count + ')</span>';
  }
}

// ── Group lookup (robust, by data attribute) ───────────────────────────────

function findGroupByTicker(feedFull, ticker) {
  for (var i = 0; i < feedFull.children.length; i++) {
    if (feedFull.children[i].dataset.ticker === ticker) return feedFull.children[i];
  }
  return null;
}

// ── Pruning ────────────────────────────────────────────────────────────────

function pruneMainFeed(feed, allSignals) {
  // Trim overflow
  while (feed.children.length > MAX_FEED) {
    feed.removeChild(feed.lastChild);
  }

  // Rebuild rendered keys from current data
  _renderedSigKeys.clear();
  for (var i = 0; i < allSignals.length; i++) {
    _renderedSigKeys.add(sigKey(allSignals[i]));
  }

  // Remove DOM items no longer in data
  for (var i = feed.children.length - 1; i >= 0; i--) {
    var sk = feed.children[i].dataset.sk;
    if (sk && !_renderedSigKeys.has(sk)) feed.removeChild(feed.children[i]);
  }
}

function pruneFullFeed(feedFull, allSignals) {
  // Trim per-group overflow
  for (var i = 0; i < feedFull.children.length; i++) {
    var bodyEl = feedFull.children[i].querySelector('.sig-group-body');
    if (bodyEl && bodyEl.children.length > MAX_FEED) {
      while (bodyEl.children.length > MAX_FEED) {
        bodyEl.removeChild(bodyEl.lastChild);
      }
    }
  }

  // Rebuild rendered keys
  _renderedSigFullKeys.clear();
  for (var i = 0; i < allSignals.length; i++) {
    _renderedSigFullKeys.add(sigKey(allSignals[i]));
  }

  // Remove stale DOM items and empty groups
  for (var i = feedFull.children.length - 1; i >= 0; i--) {
    var grp = feedFull.children[i];
    var bodyEl = grp.querySelector('.sig-group-body');
    if (!bodyEl) continue;
    for (var j = bodyEl.children.length - 1; j >= 0; j--) {
      var sk = bodyEl.children[j].dataset.sk;
      if (sk && !_renderedSigFullKeys.has(sk)) bodyEl.removeChild(bodyEl.children[j]);
    }
    if (bodyEl.children.length === 0) feedFull.removeChild(grp);
  }
}

// ── Sub-renderers ──────────────────────────────────────────────────────────

function updateBadge(sigs) {
  var badge = $('sig-tab-badge');
  if (!badge) return;
  var newCount = 0;
  for (var i = 0; i < sigs.length; i++) {
    if (!_seenSignals.has(sigKey(sigs[i]))) newCount++;
  }
  badge.textContent = newCount;
  badge.style.display = newCount ? 'inline' : 'none';
}

function renderMainFeed(filtered) {
  var feed = $('sig-feed');
  if (!feed) return;

  var newCount = countConsecutiveNew(filtered, _renderedSigKeys);

  if (filtered.length === 0) {
    feed.replaceChildren();
    return;
  }

  if (newCount >= filtered.length || feed.children.length === 0) {
    // ── Full render ──
    feed.replaceChildren();
    var limit = filtered.length < MAX_FEED ? filtered.length : MAX_FEED;
    for (var i = 0; i < limit; i++) {
      var s = filtered[i];
      var key = sigKey(s);
      _renderedSigKeys.add(key);
      var isNew = !_seenSignals.has(key);
      feed.appendChild(makeFeedItem(s, { isNew: isNew, showTicker: true }));
    }
  } else if (newCount > 0) {
    // ── Prepend new items ──
    var atBottom = feed.scrollHeight - feed.scrollTop <= feed.clientHeight + 40;
    for (var i = newCount - 1; i >= 0; i--) {
      var s = filtered[i];
      var key = sigKey(s);
      _renderedSigKeys.add(key);
      var isNew = !_seenSignals.has(key);
      feed.insertBefore(
        makeFeedItem(s, { isNew: isNew, showTicker: true, extraClass: 'signal-new' }),
        feed.firstChild
      );
    }
    if (atBottom) feed.scrollTop = 0;
  }

  pruneMainFeed(feed, filtered);
}

function renderFullFeed(filtered) {
  var feedFull = $('sig-feed-full');
  if (!feedFull) return;

  var newCount = countConsecutiveNew(filtered, _renderedSigFullKeys);

  if (filtered.length === 0) {
    feedFull.replaceChildren();
    return;
  }

  if (newCount >= filtered.length || feedFull.children.length === 0) {
    // ── Full render ──
    feedFull.replaceChildren();

    // Group by ticker
    var groups = new Map();
    for (var i = 0; i < filtered.length; i++) {
      var s = filtered[i];
      if (!groups.has(s.ticker)) groups.set(s.ticker, []);
      groups.get(s.ticker).push(s);
    }

    var entries = Array.from(groups.entries()).sort(function (a, b) {
      return a[0].localeCompare(b[0]);
    });
    for (var ei = 0; ei < entries.length; ei++) {
      var ticker = entries[ei][0];
      var gsigs = entries[ei][1];

      var grp = el('div', 'sig-group');
      grp.dataset.ticker = ticker;

      var head = makeGroupHeader(ticker, gsigs.length);

      var bodyEl = el('div', 'sig-group-body');
      if (_groupOpen['sig-grp-' + ticker]) bodyEl.classList.add('open');

      for (var si = 0; si < gsigs.length; si++) {
        var s = gsigs[si];
        var key = sigKey(s);
        _renderedSigFullKeys.add(key);
        var isNew = !_seenSignals.has(key);
        bodyEl.appendChild(
          makeFeedItem(s, { isNew: isNew, showTicker: false, showConfidence: true })
        );
      }

      bindGroupToggle(head, bodyEl, ticker);
      grp.appendChild(head);
      grp.appendChild(bodyEl);
      feedFull.appendChild(grp);
    }
  } else if (newCount > 0) {
    // ── Incremental: prepend new signals ──
    var atBottom =
      feedFull.scrollHeight - feedFull.scrollTop <= feedFull.clientHeight + 40;

    // Group new signals by ticker
    var newByTicker = new Map();
    for (var i = 0; i < newCount; i++) {
      var s = filtered[i];
      if (!newByTicker.has(s.ticker)) newByTicker.set(s.ticker, []);
      newByTicker.get(s.ticker).push(s);
    }

    var tickers = Array.from(newByTicker.keys());
    for (var ti = 0; ti < tickers.length; ti++) {
      var ticker = tickers[ti];
      var newSigs = newByTicker.get(ticker);
      var grp = findGroupByTicker(feedFull, ticker);

      if (grp) {
        // Existing group — prepend to its body
        var bodyEl = grp.querySelector('.sig-group-body');
        var head = grp.querySelector('.sig-group-head');
        if (bodyEl && head) {
          for (var i = newSigs.length - 1; i >= 0; i--) {
            var s = newSigs[i];
            var key = sigKey(s);
            _renderedSigFullKeys.add(key);
            var isNew = !_seenSignals.has(key);
            bodyEl.insertBefore(
              makeFeedItem(s, {
                isNew: isNew,
                showTicker: false,
                showConfidence: true,
                extraClass: 'signal-new',
              }),
              bodyEl.firstChild
            );
          }
          updateGroupCount(head, ticker, bodyEl.children.length);
        }
      } else {
        // New ticker — create group
        grp = el('div', 'sig-group');
        grp.dataset.ticker = ticker;
        _groupOpen['sig-grp-' + ticker] = true;

        var head = makeGroupHeader(ticker, newSigs.length);

        var bodyEl = el('div', 'sig-group-body');
        bodyEl.classList.add('open');

        for (var i = newSigs.length - 1; i >= 0; i--) {
          var s = newSigs[i];
          var key = sigKey(s);
          _renderedSigFullKeys.add(key);
          var isNew = !_seenSignals.has(key);
          bodyEl.appendChild(
            makeFeedItem(s, {
              isNew: isNew,
              showTicker: false,
              showConfidence: true,
              extraClass: 'signal-new',
            })
          );
        }

        bindGroupToggle(head, bodyEl, ticker);
        grp.appendChild(head);
        grp.appendChild(bodyEl);
        feedFull.insertBefore(grp, feedFull.firstChild);
      }
    }

    if (atBottom) feedFull.scrollTop = feedFull.scrollHeight;
  }

  pruneFullFeed(feedFull, filtered);
}

/**
 * Mark all current signals as seen — called once, at the end,
 * so the badge only highlights signals that arrive between polls.
 */
function markAllSeen(sigs) {
  if ($('tab-signals') && $('tab-signals').classList.contains('active')) {
    for (var i = 0; i < sigs.length; i++) {
      _seenSignals.add(sigKey(sigs[i]));
    }
  }

  if (_seenSignals.size > 10000) {
    var arr = Array.from(_seenSignals);
    _seenSignals = new Set(arr.slice(arr.length - 5000));
  }
}

// ── Public API ─────────────────────────────────────────────────────────────

function renderSignals(data) {
  var sigs = data.recent_signals || [];
  var filtered = _sigFilter ? sigs.filter(function (s) { return s.kind === _sigFilter; }) : sigs;

  updateBadge(sigs);
  renderMainFeed(filtered);
  renderFullFeed(filtered);
  markAllSeen(sigs);

  buildSignalFilters(data);
  renderSigCounts(data);
}

// ── Filter chips ───────────────────────────────────────────────────────────

function buildSignalFilters(data) {
  var container = $('sig-filter-row');
  if (!container) return;
  var sigs = data.recent_signals || [];
  var kinds = ['ALL'].concat(Array.from(new Set(sigs.map(function (s) { return s.kind; })))).sort();

  var kindsKey = JSON.stringify(kinds);
  if (_lastSigFilter === kindsKey) return;
  setLastSigFilter(kindsKey);

  container.replaceChildren();
  kinds.forEach(function (k) {
    var chip = el('span', 'filter-chip', k);
    if (_sigFilter === k || (_sigFilter === '' && k === 'ALL')) chip.classList.add('active');
    chip.addEventListener('click', function () {
      setSigFilter(k === 'ALL' ? '' : k);
      document
        .querySelectorAll('#sig-filter-row .filter-chip')
        .forEach(function (c) { c.classList.remove('active'); });
      chip.classList.add('active');
      renderSignals(data);
    });
    container.appendChild(chip);
  });
}

// ── Signal counts table ────────────────────────────────────────────────────

function renderSigCounts(data) {
  var body = $('sig-cnt-body');
  if (!body) return;
  var counts = data.signal_counts || {};
  var sorted = Object.entries(counts).sort(function (a, b) { return b[1] - a[1]; });
  body.replaceChildren();
  if (!sorted.length) {
    body.innerHTML = '<div class="empty-state">No signals yet</div>';
    return;
  }

  sorted.forEach(function (entry) {
    var kind = entry[0];
    var count = entry[1];
    var row = el('div', 'sig-cnt-row');
    row.innerHTML = '<span>' + kind + '</span><span class="mono">' + count + '</span>';
    body.appendChild(row);
  });
}
