#include "DashboardServer.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>

#include <cctype>
#include <deque>

namespace trade_bot {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

static const char* kDashboardHtml = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Trade Bot HQ</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<style>
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&family=JetBrains+Mono:wght@400;600&display=swap');
:root {
  --bg: #07080d; --surface: #0d1117; --surface-glass: rgba(15,20,30,0.55);
  --surface-raised: #151b27; --surface-hover: rgba(255,255,255,0.04);
  --border: rgba(255,255,255,0.06); --border-bright: rgba(255,255,255,0.1);
  --text: #e8ecf4; --muted: #7d8694; --subtle: #3d4450;
  --accent: #7c5cfc; --accent2: #4facfe;
  --positive: #00e676; --negative: #ff5252; --warning: #ffab00; --info: #40c4ff;
  --positive-soft: rgba(0,230,118,0.1); --negative-soft: rgba(255,82,82,0.1);
  --glow-pos: 0 0 16px rgba(0,230,118,0.25); --glow-neg: 0 0 16px rgba(255,82,82,0.25);
  --radius: 12px; --radius-sm: 8px;
  --font: 'Inter', -apple-system, sans-serif;
  --mono: 'JetBrains Mono', 'SF Mono', monospace;
  --transition: 0.3s cubic-bezier(0.4,0,0.2,1);
}
* { box-sizing: border-box; margin: 0; }
body {
  background: var(--bg); color: var(--text); font-family: var(--font);
  font-size: 13px; line-height: 1.5; height: 100vh; overflow: hidden;
  display: flex; flex-direction: column; font-feature-settings: "tnum";
}
::-webkit-scrollbar { width: 6px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: var(--subtle); border-radius: 3px; }

header {
  padding: clamp(8px,1vw,14px) clamp(12px,2vw,24px);
  background: var(--surface-glass); backdrop-filter: blur(20px) saturate(180%);
  border-bottom: 1px solid var(--border);
  display: flex; justify-content: space-between; align-items: center; z-index: 100;
  position: relative;
}
header::after {
  content: ''; position: absolute; bottom: -1px; left: 0; right: 0; height: 1px;
  background: linear-gradient(90deg, transparent, var(--accent), var(--accent2), transparent);
  opacity: 0.5;
}
.brand { display: flex; align-items: center; gap: 10px; font-weight: 700; font-size: 15px; letter-spacing: -0.02em; }
.brand-icon {
  width: 8px; height: 22px; border-radius: 3px;
  background: linear-gradient(180deg, var(--accent), var(--accent2));
  box-shadow: 0 0 12px rgba(124,92,252,0.4);
}
.header-right { display: flex; align-items: center; gap: clamp(8px,1.5vw,16px); }
.status-pill {
  display: inline-flex; align-items: center; gap: 6px;
  padding: 4px 14px; border-radius: 99px;
  background: var(--surface-glass); backdrop-filter: blur(10px);
  border: 1px solid var(--border); font-size: 11px; font-weight: 600; text-transform: uppercase;
}
.dot { width: 8px; height: 8px; border-radius: 50%; background: var(--subtle); transition: all var(--transition); }
.dot.live { background: var(--positive); box-shadow: var(--glow-pos); animation: pulse 2s infinite; }
@keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.5; } }
.clock { font-family: var(--mono); font-size: 11px; color: var(--muted); }

.stats-bar {
  display: flex; gap: clamp(6px,1vw,12px); padding: clamp(8px,1vw,12px) clamp(12px,2vw,24px);
  background: var(--surface); border-bottom: 1px solid var(--border);
  overflow-x: auto; flex-shrink: 0;
}
.kpi {
  display: flex; flex-direction: column; gap: 2px;
  padding: clamp(6px,0.8vw,10px) clamp(10px,1.2vw,16px);
  background: var(--surface-glass); backdrop-filter: blur(12px);
  border: 1px solid var(--border); border-radius: var(--radius-sm);
  min-width: 120px; flex: 1;
  transition: border-color var(--transition), box-shadow var(--transition);
}
.kpi:hover { border-color: var(--border-bright); box-shadow: 0 4px 20px rgba(0,0,0,0.3); }
.kpi-label { font-size: 10px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; font-weight: 600; }
.kpi-value { font-size: clamp(16px,1.8vw,22px); font-weight: 800; font-family: var(--mono); transition: color var(--transition), text-shadow var(--transition); }

main {
  flex: 1; display: grid;
  grid-template-columns: minmax(260px, 22vw) 1fr;
  gap: 1px; background: var(--border); overflow: hidden;
}
.panel { background: var(--bg); display: flex; flex-direction: column; overflow: hidden; }
.scrollable { overflow-y: auto; flex: 1; padding: clamp(8px,1vw,12px); display: flex; flex-direction: column; gap: clamp(8px,1vw,12px); }

.card {
  background: var(--surface-glass); backdrop-filter: blur(16px) saturate(180%);
  border: 1px solid var(--border); border-radius: var(--radius);
  overflow: hidden; transition: border-color var(--transition), box-shadow var(--transition);
}
.card:hover { border-color: var(--border-bright); }
.card-head {
  padding: clamp(8px,1vw,12px) clamp(12px,1.5vw,16px);
  display: flex; justify-content: space-between; align-items: center;
  cursor: pointer; user-select: none; transition: background var(--transition);
}
.card-head:hover { background: var(--surface-hover); }
.card-head h2 {
  font-size: 11px; font-weight: 700; color: var(--muted);
  text-transform: uppercase; letter-spacing: 0.06em;
}
.card-chevron {
  width: 16px; height: 16px; color: var(--subtle);
  transition: transform var(--transition);
}
.card.collapsed .card-chevron { transform: rotate(-90deg); }
.card-body {
  padding: clamp(8px,1vw,12px) clamp(12px,1.5vw,16px);
  max-height: 2000px; overflow: hidden;
  transition: max-height 0.4s ease, padding 0.3s ease, opacity 0.3s ease; opacity: 1;
}
.card.collapsed .card-body { max-height: 0; padding-top: 0; padding-bottom: 0; opacity: 0; }

.stat-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(100px, 1fr)); gap: 8px; }
.stat-item {
  padding: 10px; background: rgba(255,255,255,0.02); border-radius: var(--radius-sm);
  border: 1px solid rgba(255,255,255,0.03);
}
.stat-item .stat-label { font-size: 9px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 4px; }
.stat-item .stat-value { font-size: 15px; font-weight: 700; font-family: var(--mono); }

.donut-box { display: flex; align-items: center; justify-content: center; padding: 8px 0; gap: 14px; }
.donut-ring { fill: none; stroke-width: 8; transform: rotate(-90deg); transform-origin: 50% 50%; }

.chart-box { height: clamp(100px,12vw,160px); position: relative; margin-top: 8px; }
.chart-box svg { width: 100%; height: 100%; overflow: visible; }
.equity-line { fill: none; stroke: var(--accent2); stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
.equity-area { fill: url(#eq-grad); }
.chart-grid { stroke: rgba(255,255,255,0.04); stroke-width: 1; }
.chart-label { fill: var(--subtle); font-size: 9px; font-family: var(--mono); }

.section { overflow: hidden; }
.section-head {
  padding: clamp(8px,1vw,10px) clamp(12px,1.5vw,16px);
  background: var(--surface-raised); border-bottom: 1px solid var(--border);
  display: flex; justify-content: space-between; align-items: center;
}
.section-head h2 { font-size: 11px; font-weight: 700; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; }

table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th {
  position: sticky; top: 0; z-index: 10; text-align: left;
  padding: clamp(6px,0.8vw,8px) clamp(10px,1.2vw,16px);
  background: var(--surface-raised); color: var(--muted);
  font-size: 10px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.04em;
  border-bottom: 1px solid var(--border); cursor: pointer; user-select: none;
  transition: color var(--transition);
}
th:hover { color: var(--text); }
th.sort-asc::after { content: ' ↑'; color: var(--accent); }
th.sort-desc::after { content: ' ↓'; color: var(--accent); }
td {
  padding: clamp(6px,0.8vw,8px) clamp(10px,1.2vw,16px);
  border-bottom: 1px solid rgba(255,255,255,0.03);
  font-size: 12px; transition: background var(--transition);
}
tr { transition: background var(--transition); }
tr:hover td { background: var(--surface-hover); }
tr.fade-in { animation: fadeRow 0.4s ease-out; }
@keyframes fadeRow { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; transform: translateY(0); } }

.progress-track { height: 4px; background: rgba(255,255,255,0.06); border-radius: 2px; overflow: hidden; margin-top: 4px; }
.progress-fill { height: 100%; transition: width 0.5s ease; border-radius: 2px; }

#toasts { position: fixed; bottom: 20px; right: 20px; display: flex; flex-direction: column; gap: 8px; z-index: 1000; pointer-events: none; }
.toast {
  background: var(--surface-glass); backdrop-filter: blur(20px);
  border: 1px solid var(--border); border-left: 3px solid var(--info);
  padding: 12px 16px; border-radius: var(--radius-sm); box-shadow: 0 8px 32px rgba(0,0,0,0.5);
  animation: toastIn 0.4s cubic-bezier(0.34,1.56,0.64,1); min-width: 260px; pointer-events: auto;
}
@keyframes toastIn { from { transform: translateX(100%) scale(0.8); opacity: 0; } to { transform: translateX(0) scale(1); opacity: 1; } }

.feed {
  flex: 1; overflow-y: auto; font-family: var(--mono); font-size: 11px;
  padding: 8px clamp(10px,1.2vw,16px); background: rgba(0,0,0,0.3); color: #99a; min-height: 80px;
  border-radius: 0 0 var(--radius) var(--radius);
}
.feed-line { margin-bottom: 2px; white-space: pre-wrap; }
.lvl-err { color: var(--negative); } .lvl-wrn { color: var(--warning); } .lvl-inf { color: var(--info); }

.skeleton {
  background: linear-gradient(90deg, var(--surface) 25%, var(--surface-raised) 50%, var(--surface) 75%);
  background-size: 200% 100%; animation: loading 1.5s infinite;
  height: 12px; border-radius: 4px; width: 100%;
}
@keyframes loading { 0% { background-position: 200% 0; } 100% { background-position: -200% 0; } }

.val-up { color: var(--positive); } .val-dn { color: var(--negative); }
.badge { padding: 2px 8px; border-radius: 6px; font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.03em; }
.bg-buy { background: var(--positive-soft); color: var(--positive); }
.bg-sell { background: var(--negative-soft); color: var(--negative); }
.badge-count { background: rgba(124,92,252,0.15); color: var(--accent); padding: 2px 8px; border-radius: 6px; font-size: 11px; font-weight: 700; }
.ks-badge { background: rgba(255,82,82,0.2); color: var(--negative); border: 1px solid rgba(255,82,82,0.3); animation: pulse 1.5s infinite; font-weight: 700; font-size: 11px; padding: 4px 12px; border-radius: 6px; }

@media (max-width: 1024px) {
  main { grid-template-columns: 1fr; }
  .panel-sidebar { order: 2; max-height: 40vh; }
  .panel-sidebar .scrollable { flex-direction: row; flex-wrap: wrap; }
  .panel-sidebar .card { min-width: 240px; flex: 1; }
}
@media (max-width: 640px) {
  .stats-bar { flex-wrap: wrap; }
  .kpi { min-width: 100px; }
  .panel-sidebar .scrollable { flex-direction: column; }
  .panel-sidebar .card { min-width: unset; }
  header { flex-wrap: wrap; gap: 8px; }
}

</style>
</head>
<body>
<div id="toasts"></div>
<header>
  <div class="brand"><div class="brand-icon"></div> TradeBot <span id="v-tag" style="color:var(--muted);font-weight:400;">v0.0.0</span></div>
  <div class="header-right">
    <div id="ks-tag" class="ks-badge" style="display:none;">⚠ KILL SWITCH</div>
    <div class="clock" id="clock"></div>
    <div class="status-pill"><div id="ws-dot" class="dot"></div> <span id="ws-label">Offline</span></div>
  </div>
</header>
<div class="stats-bar">
  <div class="kpi"><div class="kpi-label">💰 Equity</div><div class="kpi-value" id="equity-val">$0.00</div></div>
  <div class="kpi"><div class="kpi-label">📈 PnL Today</div><div class="kpi-value" id="pnl-today">$0.00</div></div>
  <div class="kpi"><div class="kpi-label">📉 Max DD</div><div class="kpi-value" id="m-mdd">0.00%</div></div>
  <div class="kpi"><div class="kpi-label">🏆 Win Rate</div><div class="kpi-value" id="wr-val">0%</div></div>
  <div class="kpi"><div class="kpi-label">⚖️ Prof. Factor</div><div class="kpi-value" id="m-pf">0.00</div></div>
  <div class="kpi"><div class="kpi-label">🔥 Win Streak</div><div class="kpi-value" id="m-sw">0</div></div>
  <div class="kpi"><div class="kpi-label">❄️ Loss Streak</div><div class="kpi-value" id="m-sl">0</div></div>
</div>
<main>
  <div class="panel panel-sidebar">
    <div class="scrollable">
      <div class="card" id="card-equity">
        <div class="card-head" onclick="toggleCard(this)"><h2>Equity Curve</h2><svg class="card-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6 9 12 15 18 9"/></svg></div>
        <div class="card-body">
          <div class="donut-box">
            <svg width="70" height="70" viewBox="0 0 100 100" id="wr-svg"></svg>
            <div><div id="wr-donut-val" style="font-size:18px;font-weight:800;font-family:var(--mono);">0%</div><div style="font-size:9px;color:var(--muted);text-transform:uppercase;letter-spacing:0.05em;">Win Rate</div></div>
          </div>
          <div class="chart-box" id="eq-chart"></div>
        </div>
      </div>
      <div class="card" id="card-universe">
        <div class="card-head" onclick="toggleCard(this)"><h2>Universe</h2><svg class="card-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6 9 12 15 18 9"/></svg></div>
        <div class="card-body" style="padding:0;">
          <table id="uni-table">
            <thead><tr><th data-key="ticker">Ticker</th><th data-key="mark">Mark</th><th>Strategies</th></tr></thead>
            <tbody id="uni-body"></tbody>
          </table>
        </div>
      </div>
      <div class="card" id="card-signals">
        <div class="card-head" onclick="toggleCard(this)"><h2>Signal Counts</h2><svg class="card-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6 9 12 15 18 9"/></svg></div>
        <div class="card-body" style="padding:0;">
          <table id="sig-cnt-tbl"><tbody id="sig-cnt-body"></tbody></table>
        </div>
      </div>
    </div>
  </div>
  <div class="panel">
    <div class="scrollable" style="gap:0;">
      <div class="card" style="border-radius:var(--radius);margin-bottom:clamp(8px,1vw,12px);">
        <div class="section-head"><h2>Active Positions</h2><span id="pos-n" class="badge-count">0</span></div>
        <div style="overflow-x:auto;">
          <table id="pos-tbl">
            <thead><tr>
              <th data-key="ticker">Symbol</th><th data-key="side">Side</th><th data-key="entry">Entry</th>
              <th data-key="mark">Mark</th><th data-key="upnl">uPnL</th><th data-key="dist">Stop Dist</th><th data-key="age">Age</th>
            </tr></thead>
            <tbody id="pos-body"></tbody>
          </table>
          <div id="pos-empty" style="padding:32px;text-align:center;color:var(--subtle);font-size:12px;">No active trades</div>
        </div>
      </div>
      <div class="card" style="border-radius:var(--radius);margin-bottom:clamp(8px,1vw,12px);">
        <div class="section-head"><h2>Trade Journal</h2></div>
        <div style="overflow-x:auto;">
          <table id="jrn-tbl">
            <thead><tr>
              <th data-key="ticker">Ticker</th><th data-key="side">Side</th><th data-key="pnl">PnL</th>
              <th data-key="entry">Entry</th><th data-key="exit">Exit</th><th data-key="reason">Reason</th>
            </tr></thead>
            <tbody id="jrn-body"></tbody>
          </table>
        </div>
      </div>
      <div class="card" style="border-radius:var(--radius);margin-bottom:clamp(8px,1vw,12px);">
        <div class="card-head" onclick="toggleCard(this)"><h2>Live Signals</h2><svg class="card-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6 9 12 15 18 9"/></svg></div>
        <div class="card-body" style="padding:0;"><div class="feed" id="sig-feed"></div></div>
      </div>
      <div class="card" style="border-radius:var(--radius);">
        <div class="card-head" onclick="toggleCard(this)"><h2>System Logs</h2><svg class="card-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="6 9 12 15 18 9"/></svg></div>
        <div class="card-body" style="padding:0;"><div class="feed" id="log-feed"></div></div>
      </div>
    </div>
  </div>
</main>

<script>
'use strict';
const NS = "http://www.w3.org/2000/svg";
let _state = null, _prevTrades = new Map(), _openedAt = new Map();
let _sort = { tbl: 'pos-table', key: 'upnl', dir: -1 };
let _jrnHash = '', _uniHash = '', _sigHash = '', _posKeys = [];

function $(id){ return document.getElementById(id); }
function el(tag, cls, txt){
  const e = document.createElement(tag);
  if(cls) e.className = cls;
  if(txt !== undefined) e.textContent = txt;
  return e;
}
function fmtMoney(v){ return '$' + Number(v).toLocaleString(undefined, {minimumFractionDigits:2, maximumFractionDigits:2}); }
function fmtS(v, d=2){ return (v > 0 ? '+' : '') + Number(v).toFixed(d); }
function updateClock() { const n=new Date(); $('clock').textContent='UTC '+n.toUTCString().slice(17,25); }
// Fast hash for change detection
function fastHash(arr, fn) { let h=''; for(let i=0;i<arr.length;i++) h+=fn(arr[i])+'|'; return h; }

function update(data) {
  _state = data;
  detectDiffs(_state);
  renderHeader(data);
  const m = computeMetrics(data);
  renderStatsBar(data, m);
  // Only rebuild heavy sections when data changes
  const jh = fastHash(data.recent_journal, e=>e.plan.ticker+e.pnl_usd);
  const jrnChanged = jh !== _jrnHash;
  if(jrnChanged) { _jrnHash=jh; renderJournal(data); }
  const uh = fastHash(data.universe||[], u=>u.ticker+u.mark_price);
  if(uh !== _uniHash) { _uniHash=uh; renderUniverse(data); }
  const sh = data.recent_signals.length ? data.recent_signals[0].time_str : '';
  if(sh !== _sigHash) { _sigHash=sh; renderSignals(data); }
  // Donut + chart: redraw chart only on journal change (equity curve derives from journal)
  renderDonut(m);
  if(jrnChanged || !_chartDrawn) { chart(m.series); _chartDrawn=true; }
  renderPositions(data);
}
let _chartDrawn = false;

function renderHeader(data) {
  $('v-tag').textContent = `v${data.version || '0.0.0'}`;
  $('ks-tag').style.display = data.kill_switch_active ? 'inline-flex' : 'none';
}

function renderStatsBar(data, m) {
  $('equity-val').textContent = fmtMoney(data.account.equity_usd);
  const pToday = data.account.realized_pnl_today_usd;
  const pPct = data.account.starting_equity_usd > 0 ? (pToday / data.account.starting_equity_usd) * 100 : 0;
  const pnlEl = $('pnl-today');
  pnlEl.textContent = `${fmtS(pToday)} (${fmtS(pPct)}%)`;
  pnlEl.style.color = pToday >= 0 ? 'var(--positive)' : 'var(--negative)';
  pnlEl.style.textShadow = pToday >= 0 ? 'var(--glow-pos)' : 'var(--glow-neg)';
  $('m-mdd').textContent = m.mdd.toFixed(2) + '%';
  $('wr-val').textContent = Math.round(m.wr) + '%';
  $('m-pf').textContent = m.pf.toFixed(2);
  $('m-sw').textContent = m.sw;
  $('m-sl').textContent = m.sl;
}

function renderDonut(m) {
  const wrSvg = $('wr-svg');
  wrSvg.replaceChildren();
  const r1 = document.createElementNS(NS, "circle");
  r1.setAttribute("cx","50"); r1.setAttribute("cy","50"); r1.setAttribute("r","40");
  r1.setAttribute("class","donut-ring"); r1.style.stroke = "var(--border-bright)";
  wrSvg.appendChild(r1);
  if (m.wr > 0) {
    const r2 = document.createElementNS(NS, "circle");
    r2.setAttribute("cx","50"); r2.setAttribute("cy","50"); r2.setAttribute("r","40");
    r2.setAttribute("class","donut-ring"); r2.style.stroke = "var(--positive)";
    const c = 2 * Math.PI * 40;
    r2.style.strokeDasharray = `${(m.wr/100)*c} ${c}`;
    r2.style.filter = 'drop-shadow(0 0 4px rgba(0,230,118,0.4))';
    wrSvg.appendChild(r2);
  }
  $('wr-donut-val').textContent = Math.round(m.wr) + '%';
  const scBody = $('sig-cnt-body');
  scBody.replaceChildren();
  Object.entries(_state.signal_counts).sort((a,b)=>b[1]-a[1]).slice(0,8).forEach(([k,v])=>{
    const tr = el('tr');
    tr.appendChild(el('td', 'muted', k));
    const tdV = el('td', '', v.toString()); tdV.style.textAlign='right'; tdV.style.fontWeight='700';
    tdV.style.fontFamily='var(--mono)';
    tr.appendChild(tdV);
    scBody.appendChild(tr);
  });
}

// Incremental position update — reuse existing rows, only update changed cells
function renderPositions(data) {
  const body = $('pos-body');
  const live = sortRows(data.open_trades.filter(t => t.avg_entry_price > 0), 'pos-tbl');
  $('pos-n').textContent = live.length;
  $('pos-empty').style.display = live.length ? 'none' : 'block';
  const marks = new Map(data.universe.map(u => [u.ticker, u.mark_price]));
  const newKeys = live.map(t => `${t.plan.ticker}_${t.plan.side}`);
  const keysChanged = newKeys.join(',') !== _posKeys.join(',');
  _posKeys = newKeys;
  // Full rebuild only when position set changes; otherwise update cells in-place
  if (keysChanged || body.children.length !== live.length) {
    body.replaceChildren();
    live.forEach(t => body.appendChild(buildPosRow(t, marks)));
  } else {
    live.forEach((t, i) => updatePosRow(body.children[i], t, marks));
  }
}

function buildPosRow(t, marks) {
  const tr = el('tr', 'fade-in');
  tr.appendChild(el('td', '', t.plan.ticker));
  const sTd = el('td'); sTd.appendChild(el('span', `badge bg-${t.plan.side==1?'buy':'sell'}`, t.plan.side==1?'Long':'Short'));
  tr.appendChild(sTd);
  const entTd = el('td', '', t.avg_entry_price.toFixed(5)); entTd.style.fontFamily='var(--mono)'; tr.appendChild(entTd);
  const mark = marks.get(t.plan.ticker) || 0;
  const markTd = el('td', '', mark.toFixed(5)); markTd.style.fontFamily='var(--mono)'; tr.appendChild(markTd);
  const pnl = t.unrealized_pnl;
  const pnlTd = el('td', pnl>=0?'val-up':'val-dn', fmtS(pnl));
  pnlTd.style.fontWeight='600'; pnlTd.style.fontFamily='var(--mono)';
  pnlTd.style.textShadow = pnl>=0 ? 'var(--glow-pos)' : 'var(--glow-neg)';
  tr.appendChild(pnlTd);
  tr.appendChild(buildDistCell(t, mark));
  const key = `${t.plan.ticker}_${t.plan.side}`;
  const start = _openedAt.get(key) || Date.now();
  const sec = Math.floor((Date.now() - start) / 1000);
  const ageTd = el('td', 'muted', `${Math.floor(sec/60)}m ${sec%60}s`); ageTd.style.fontFamily='var(--mono)';
  tr.appendChild(ageTd);
  return tr;
}

function updatePosRow(tr, t, marks) {
  const mark = marks.get(t.plan.ticker) || 0;
  const cells = tr.children;
  // cells: 0=ticker, 1=side, 2=entry, 3=mark, 4=pnl, 5=dist, 6=age
  if(cells.length < 7) return;
  const mv = mark.toFixed(5);
  if(cells[3].textContent !== mv) cells[3].textContent = mv;
  const pnl = t.unrealized_pnl, ps = fmtS(pnl);
  if(cells[4].textContent !== ps) {
    cells[4].textContent = ps;
    cells[4].className = pnl>=0 ? 'val-up' : 'val-dn';
    cells[4].style.textShadow = pnl>=0 ? 'var(--glow-pos)' : 'var(--glow-neg)';
  }
  // Update dist bar
  const entry = t.avg_entry_price, stop = t.plan.stop_price;
  if(entry && stop && mark) {
    const tot = Math.abs(entry-stop), cur = Math.abs(mark-stop);
    const pct = Math.max(0, Math.min(100, (cur/tot)*100));
    const bar = cells[5].querySelector('.progress-fill');
    if(bar) {
      bar.style.width = pct+'%';
      bar.style.background = pct<20?'var(--negative)':(pct<50?'var(--warning)':'var(--positive)');
      bar.style.boxShadow = pct<20?'var(--glow-neg)':'none';
    }
  }
  const key = `${t.plan.ticker}_${t.plan.side}`;
  const start = _openedAt.get(key) || Date.now();
  const sec = Math.floor((Date.now() - start) / 1000);
  cells[6].textContent = `${Math.floor(sec/60)}m ${sec%60}s`;
}

function buildDistCell(t, mark) {
  const dTd = el('td');
  const entry = t.avg_entry_price, stop = t.plan.stop_price;
  if (entry && stop && mark) {
    const tot = Math.abs(entry-stop), cur = Math.abs(mark-stop);
    const pct = Math.max(0, Math.min(100, (cur/tot)*100));
    const trk = el('div','progress-track');
    const fil = el('div','progress-fill'); fil.style.width=pct+'%';
    fil.style.background = pct<20?'var(--negative)':(pct<50?'var(--warning)':'var(--positive)');
    if(pct<20) fil.style.boxShadow='var(--glow-neg)';
    trk.appendChild(fil); dTd.appendChild(trk);
  } else dTd.textContent = '—';
  return dTd;
}

function renderJournal(data) {
  const body = $('jrn-body');
  body.replaceChildren();
  sortRows(data.recent_journal.slice(0,20), 'jrn-tbl').forEach(e => {
    const tr = el('tr');
    tr.appendChild(el('td', '', e.plan.ticker));
    const sTd = el('td');
    sTd.appendChild(el('span', `badge bg-${e.plan.side==1?'buy':'sell'}`, e.plan.side==1?'L':'S'));
    tr.appendChild(sTd);
    const pnlTd = el('td', e.pnl_usd>=0?'val-up':'val-dn', fmtS(e.pnl_usd));
    pnlTd.style.fontWeight='600'; pnlTd.style.fontFamily='var(--mono)';
    tr.appendChild(pnlTd);
    const entTd = el('td','',e.plan.entry_price.toFixed(5)); entTd.style.fontFamily='var(--mono)'; tr.appendChild(entTd);
    const exTd = el('td','',e.exit_price.toFixed(5)); exTd.style.fontFamily='var(--mono)'; tr.appendChild(exTd);
    tr.appendChild(el('td', 'muted', e.cause_of_exit || '—'));
    body.appendChild(tr);
  });
}

function renderUniverse(data) {
  const body = $('uni-body');
  if(!body) return;
  body.replaceChildren();
  sortRows(data.universe || [], 'uni-table').forEach(r => {
    const tr = el('tr');
    tr.appendChild(el('td', r.boosted?'val-up':'', r.ticker+(r.boosted?' ★':'')));
    const mt = el('td','',r.mark_price?r.mark_price.toFixed(2):'—'); mt.style.fontFamily='var(--mono)'; tr.appendChild(mt);
    tr.appendChild(el('td', 'muted', (r.strategies||[]).join(', ')));
    body.appendChild(tr);
  });
}

function renderSignals(data) {
  const feed = $('sig-feed');
  const atB = feed.scrollHeight - feed.scrollTop <= feed.clientHeight + 20;
  feed.textContent = '';
  data.recent_signals.forEach(s => {
    const l = el('div', 'feed-line');
    l.appendChild(el('span', 'muted', `[${s.time_str}] `));
    l.appendChild(el('span', 'lvl-inf', `${s.ticker} `));
    l.appendChild(el('span', 'lvl-wrn', `${s.kind} `));
    l.appendChild(el('span', '', `@${s.price.toFixed(5)} `));
    l.appendChild(el('span', 'subtle', `(${s.confidence.toFixed(2)})`));
    feed.appendChild(l);
  });
  if (atB) feed.scrollTop = feed.scrollHeight;
}

function detectDiffs(next) {
  const nMap = new Map();
  next.open_trades.filter(t => t.avg_entry_price > 0).forEach(t => {
    const k = `${t.plan.ticker}_${t.plan.side}`;
    nMap.set(k, t);
    if (!_prevTrades.has(k)) {
      _openedAt.set(k, Date.now());
      toast(`NEW POSITION: ${t.plan.ticker}`, `${t.plan.side==1?'Long':'Short'} @ ${t.avg_entry_price.toFixed(5)}`);
    }
  });
  _prevTrades.forEach((t, k) => {
    if (!nMap.has(k)) {
      const j = next.recent_journal.find(x => x.plan.ticker === t.plan.ticker);
      const p = j ? j.pnl_usd : 0;
      toast(`CLOSED: ${t.plan.ticker}`, `PnL: ${fmtS(p)}`, p>=0?'positive':'negative');
      _openedAt.delete(k);
    }
  });
  _prevTrades = nMap;
}

function toast(title, msg, color='info') {
  const t = el('div', 'toast');
  t.style.borderLeftColor = `var(--${color})`;
  const tTitle = el('div', '', title); tTitle.style.fontWeight='600'; tTitle.style.fontSize='12px';
  t.appendChild(tTitle);
  const m = el('div', '', msg); m.style.fontSize = '11px'; m.style.color = 'var(--muted)'; m.style.marginTop='2px';
  t.appendChild(m);
  $('toasts').appendChild(t);
  setTimeout(() => { t.style.opacity = '0'; t.style.transform='translateX(20px)'; t.style.transition='all 0.4s'; setTimeout(() => t.remove(), 400); }, 4000);
}

function computeMetrics(data) {
  const j = data.recent_journal || [], start = data.account.starting_equity_usd || data.account.equity_usd;
  let cur = start, peak = start, mdd = 0, wins = [], loss = [], sw = 0, sl = 0, cw = 0, cl = 0;
  const series = [start];
  [...j].reverse().forEach(e => {
    cur += e.pnl_usd; series.push(cur);
    if (cur > peak) peak = cur;
    const d = peak > 0 ? (peak - cur) / peak : 0;
    if (d > mdd) mdd = d;
    if (e.pnl_usd > 0) { wins.push(e.pnl_usd); cw++; sl = Math.max(sl, cl); cl = 0; }
    else if (e.pnl_usd < 0) { loss.push(Math.abs(e.pnl_usd)); cl++; sw = Math.max(sw, cw); cw = 0; }
  });
  const gW = wins.reduce((a,b)=>a+b, 0), gL = loss.reduce((a,b)=>a+b, 0);
  return { mdd: mdd*100, pf: gL>0?gW/gL:(gW>0?99:0), sw: Math.max(sw, cw), sl: Math.max(sl, cl), wr: (wins.length+loss.length)>0?(wins.length/(wins.length+loss.length))*100:0, series };
}

function chart(series) {
  const c = $('eq-chart'); c.replaceChildren();
  if (series.length < 2) return;
  const svg = document.createElementNS(NS, "svg");
  const w = c.clientWidth, h = c.clientHeight;
  if(!w || !h) return;
  const min = Math.min(...series), max = Math.max(...series), rng = max - min || 1;
  const defs = document.createElementNS(NS, "defs");
  const grad = document.createElementNS(NS, "linearGradient");
  grad.setAttribute("id", "eq-grad"); grad.setAttribute("x1", "0"); grad.setAttribute("y1", "0");
  grad.setAttribute("x2", "0"); grad.setAttribute("y2", "1");
  const s1 = document.createElementNS(NS, "stop"); s1.setAttribute("offset", "0%"); s1.setAttribute("stop-color", "rgba(79,172,254,0.3)");
  const s2 = document.createElementNS(NS, "stop"); s2.setAttribute("offset", "100%"); s2.setAttribute("stop-color", "rgba(79,172,254,0.0)");
  grad.append(s1, s2); defs.appendChild(grad); svg.appendChild(defs);
  for(let i=0; i<4; i++) {
    const gy = (h/4)*i + h/8;
    const gl = document.createElementNS(NS, "line");
    gl.setAttribute("x1","0"); gl.setAttribute("x2",w.toString()); gl.setAttribute("y1",gy.toString()); gl.setAttribute("y2",gy.toString());
    gl.setAttribute("class","chart-grid"); svg.appendChild(gl);
  }
  const lMax = document.createElementNS(NS, "text"); lMax.setAttribute("x","4"); lMax.setAttribute("y","12"); lMax.setAttribute("class","chart-label");
  lMax.textContent = '$' + max.toFixed(0);
  const lMin = document.createElementNS(NS, "text"); lMin.setAttribute("x","4"); lMin.setAttribute("y",(h-4).toString()); lMin.setAttribute("class","chart-label");
  lMin.textContent = '$' + min.toFixed(0);
  svg.append(lMax, lMin);
  const pts = series.map((v, i) => `${(i/(series.length-1))*w},${h - ((v-min)/rng)*h}`);
  const area = document.createElementNS(NS, "polygon");
  area.setAttribute("points", `0,${h} ${pts.join(' ')} ${w},${h}`);
  area.setAttribute("class", "equity-area");
  const line = document.createElementNS(NS, "polyline");
  line.setAttribute("points", pts.join(' '));
  line.setAttribute("class", "equity-line");
  svg.append(area, line); c.appendChild(svg);
}

function toggleCard(h) {
  const card = h.closest('.card');
  card.classList.toggle('collapsed');
  const id = card.id;
  if(id) {
    const state = JSON.parse(localStorage.getItem('dash-collapsed') || '{}');
    state[id] = card.classList.contains('collapsed');
    localStorage.setItem('dash-collapsed', JSON.stringify(state));
  }
}

function restoreCollapsed() {
  const state = JSON.parse(localStorage.getItem('dash-collapsed') || '{}');
  Object.entries(state).forEach(([id, collapsed]) => {
    const card = $(id);
    if(card && collapsed) card.classList.add('collapsed');
  });
}

function bindSort() {
  document.querySelectorAll('th[data-key]').forEach(th => {
    th.addEventListener('click', () => {
      const tbl = th.closest('table').id;
      const key = th.dataset.key;
      if (_sort.tbl === tbl && _sort.key === key) _sort.dir *= -1;
      else { _sort.tbl = tbl; _sort.key = key; _sort.dir = 1; }
      document.querySelectorAll('th').forEach(h => h.classList.remove('sort-asc', 'sort-desc'));
      th.classList.add(_sort.dir === 1 ? 'sort-asc' : 'sort-desc');
      update(_state);
    });
  });
}

function sortRows(arr, tblId) {
  if (_sort.tbl !== tblId) return arr;
  return [...arr].sort((a, b) => {
    let va = a[_sort.key], vb = b[_sort.key];
    if (tblId === 'pos-tbl') {
      if (_sort.key === 'ticker') { va = a.plan.ticker; vb = b.plan.ticker; }
      if (_sort.key === 'side') { va = a.plan.side; vb = b.plan.side; }
      if (_sort.key === 'entry') { va = a.avg_entry_price; vb = b.avg_entry_price; }
      if (_sort.key === 'upnl') { va = a.unrealized_pnl; vb = b.unrealized_pnl; }
    }
    if (tblId === 'jrn-tbl') {
      if (_sort.key === 'ticker') { va = a.plan.ticker; vb = b.plan.ticker; }
      if (_sort.key === 'side') { va = a.plan.side; vb = b.plan.side; }
      if (_sort.key === 'pnl') { va = a.pnl_usd; vb = b.pnl_usd; }
    }
    if (va < vb) return -1 * _sort.dir;
    if (va > vb) return 1 * _sort.dir;
    return 0;
  });
}

function connect() {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => { $('ws-dot').classList.add('live'); $('ws-label').textContent = 'Live'; };
  ws.onclose = () => { $('ws-dot').classList.remove('live'); $('ws-label').textContent = 'Offline'; setTimeout(connect, 2000); };
  ws.onmessage = e => update(JSON.parse(e.data));
}

function fetchLogs() {
  fetch('/api/logs?n=80').then(r => r.json()).then(lines => {
    const f = $('log-feed'); const atB = f.scrollHeight - f.scrollTop <= f.clientHeight + 20;
    f.replaceChildren();
    lines.forEach(l => {
      const d = el('div', 'feed-line', l);
      if (l.includes('[error]')) d.classList.add('lvl-err');
      else if (l.includes('[warn]')) d.classList.add('lvl-wrn');
      else if (l.includes('[info]')) d.classList.add('lvl-inf');
      f.appendChild(d);
    });
    if (atB) f.scrollTop = f.scrollHeight;
  }).catch(()=>{});
}

window.onload = () => {
  restoreCollapsed();
  ['pos-body', 'jrn-body', 'sig-cnt-body', 'uni-body'].forEach(id => {
    const b = $(id);
    for(let i=0; i<3; i++) {
      const tr = el('tr'); const td = el('td'); td.colSpan = 10;
      td.appendChild(el('div', 'skeleton')); tr.appendChild(td); b.appendChild(tr);
    }
  });
  bindSort();
  connect();
  updateClock(); setInterval(updateClock, 1000);
  setInterval(fetchLogs, 2000);
  window.addEventListener('keydown', e => {
    if (e.target.tagName === 'INPUT') return;
    const k = e.key.toLowerCase();
    if (k === 'l') toggleCard($('log-feed').closest('.card').querySelector('.card-head'));
    if (k === 's') toggleCard($('sig-feed').closest('.card').querySelector('.card-head'));
    if (k === 'k') window.scrollTo({top: 0, behavior: 'smooth'});
  });
};

</script>
</body>
</html>
)html";

struct DashboardServer::Session : public std::enable_shared_from_this<DashboardServer::Session> {
    websocket::stream<beast::tcp_stream> ws;
    DashboardServer& parent;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};

    Session(tcp::socket socket, DashboardServer& p) : ws(std::move(socket)), parent(p) {}

    void start(const http::request<http::string_body>& req) {
        ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (!ec) self->send_initial();
        });
    }

    void send_initial() {
        std::string payload;
        {
            std::lock_guard lock(parent.mutex_);
            payload = parent.serialize_state_locked_();
        }
        send_payload(std::move(payload));
    }

    void send_payload(std::string payload) {
        auto buf = std::make_shared<std::string>(std::move(payload));
        net::post(ws.get_executor(),
            [self = shared_from_this(), buf]() {
                self->write_queue_.push_back(buf);
                if (!self->writing_) self->do_write_();
            });
    }

    void do_write_() {
        if (write_queue_.empty()) { writing_ = false; return; }
        writing_ = true;
        auto& front = write_queue_.front();
        ws.async_write(net::buffer(*front),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    self->write_queue_.clear();
                    self->writing_ = false;
                    std::lock_guard lock(self->parent.mutex_);
                    self->parent.sessions_.erase(self);
                    return;
                }
                self->write_queue_.pop_front();
                self->do_write_();
            });
    }
};

struct HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    std::shared_ptr<http::request<http::string_body>> req;
    DashboardServer& parent;

    HttpSession(tcp::socket socket, DashboardServer& p) : stream(std::move(socket)), parent(p) {}

    void run() {
        req = std::make_shared<http::request<http::string_body>>();
        http::async_read(stream, buffer, *req, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) return;
            self->handle_request();
        });
    }

    void handle_request();
};

DashboardServer::DashboardServer(net::io_context& ioc,
                                 const std::string& address,
                                 uint16_t port,
                                 std::string auth_token)
    : ioc_(ioc)
    , acceptor_(ioc, {net::ip::make_address(address), port})
    , auth_token_(std::move(auth_token)) {
    if (auth_token_.empty() && address != "127.0.0.1" && address != "::1") {
        LOG_WARN("DashboardServer bound to {} with NO auth token — anyone on "
                 "the network can read account state. Set "
                 "[dashboard].auth_token or bind to 127.0.0.1.", address);
    }
}

bool DashboardServer::authorize(std::string_view header_value) const noexcept {
    if (auth_token_.empty()) return true;
    constexpr std::string_view prefix = "Bearer ";
    if (header_value.size() <= prefix.size() ||
        header_value.substr(0, prefix.size()) != prefix) return false;
    auto presented = header_value.substr(prefix.size());
    if (presented.size() != auth_token_.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < presented.size(); ++i)
        diff |= static_cast<unsigned>(presented[i] ^ auth_token_[i]);
    return diff == 0;
}

void DashboardServer::set_recorder(DumpRecorder* recorder) { recorder_ = recorder; }
bool DashboardServer::recorder_start(const std::string& path) { return recorder_ && recorder_->start(path); }
void DashboardServer::recorder_stop() { if (recorder_) recorder_->stop(); }
bool DashboardServer::recorder_active() const noexcept { return recorder_ && recorder_->is_active(); }
void DashboardServer::start() { do_accept(); }

void DashboardServer::update_state(const State& state) {
    std::string payload;
    std::vector<std::shared_ptr<Session>> snap;
    {
        std::lock_guard lock(mutex_);
        current_state_ = state;
        // Skip expensive JSON serialization if nobody is watching
        if (sessions_.empty()) return;
        payload = serialize_state_locked_();
        snap.assign(sessions_.begin(), sessions_.end());
    }
    for (auto& s : snap) s->send_payload(payload);
}

std::string DashboardServer::serialize_state() const {
    std::lock_guard lock(mutex_);
    return serialize_state_locked_();
}

std::size_t DashboardServer::session_count() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

std::string DashboardServer::serialize_state_locked_() const {
    nlohmann::json j;
    const auto& s = current_state_;
    j["version"]            = s.version;
    j["kill_switch_active"] = s.kill_switch_active;
    j["account"] = {
        {"equity_usd",              s.account.equity_usd},
        {"realized_pnl_today_usd",  s.account.realized_pnl_today_usd},
        {"unrealized_pnl_usd",      s.account.unrealized_pnl_usd},
        {"free_balance_usd",        s.account.free_balance_usd},
        {"starting_equity_usd",     s.account.starting_equity_usd}
    };
    j["signal_counts"] = s.signal_counts;
    j["open_trades"]   = nlohmann::json::array();
    for (const auto& t : s.open_trades) {
        j["open_trades"].push_back({
            {"plan", {
                {"ticker",        t.plan.ticker},
                {"side",          static_cast<int>(t.plan.side)},
                {"strategy_name", t.plan.strategy_name},
                {"stop_price",    t.plan.stop_price},
                {"tp1_price",     t.plan.tp1_price}
            }},
            {"executed_size",   t.executed_size},
            {"avg_entry_price", t.avg_entry_price},
            {"unrealized_pnl",  t.unrealized_pnl}
        });
    }
    j["recent_journal"] = nlohmann::json::array();
    for (const auto& e : s.recent_journal) {
        j["recent_journal"].push_back({
            {"plan", {
                {"ticker",        e.plan.ticker},
                {"strategy_name", e.plan.strategy_name},
                {"size_coin",     e.plan.size_coin},
                {"side",          static_cast<int>(e.plan.side)},
                {"entry_price",   e.plan.entry_price}
            }},
            {"pnl_usd",       e.pnl_usd},
            {"exit_price",    e.exit_price},
            {"cause_of_exit", e.cause_of_exit}
        });
    }
    j["dump_recorder"] = {
        {"active", recorder_ && recorder_->is_active()},
        {"path",   recorder_ ? recorder_->path() : ""}
    };
    j["universe"] = nlohmann::json::array();
    for (const auto& r : s.universe_rows) {
        j["universe"].push_back({
            {"ticker",     r.ticker},
            {"strategies", r.strategies},
            {"boosted",    r.boosted},
            {"mark_price", r.mark_price}
        });
    }
    j["recent_signals"] = nlohmann::json::array();
    for (const auto& sg : s.recent_signals) {
        j["recent_signals"].push_back({
            {"kind",       sg.kind},
            {"ticker",     sg.ticker},
            {"price",      sg.price},
            {"confidence", sg.confidence},
            {"time_str",   sg.time_str}
        });
    }
    return j.dump();
}

void DashboardServer::do_accept() {
    acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
        if (!ec) on_accept(ec, std::move(socket));
        do_accept();
    });
}

void DashboardServer::on_accept(beast::error_code, tcp::socket socket) {
    std::make_shared<HttpSession>(std::move(socket), *this)->run();
}

void DashboardServer::start_ws_session(tcp::socket socket,
                                        const http::request<http::string_body>& req) {
    auto ws_session = std::make_shared<Session>(std::move(socket), *this);
    {
        std::lock_guard lock(mutex_);
        sessions_.insert(ws_session);
    }
    ws_session->start(req);
}

void HttpSession::handle_request() {
    const auto auth_header = std::string(req->base()[http::field::authorization]);
    if (!parent.authorize(auth_header)) {
        auto res = std::make_shared<http::response<http::string_body>>(
            http::status::unauthorized, req->version());
        res->set(http::field::server,          BOOST_BEAST_VERSION_STRING);
        res->set(http::field::www_authenticate, "Bearer realm=\"trade_bot\"");
        res->set(http::field::content_type,    "text/plain");
        res->keep_alive(false);
        res->body() = "401 Unauthorized\n";
        res->prepare_payload();
        http::async_write(stream, *res,
            [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
        return;
    }

    if (websocket::is_upgrade(*req)) {
        parent.start_ws_session(stream.release_socket(), *req);
        return;
    }

    auto send_json = [&](http::status status, const std::string& body) {
        auto r = std::make_shared<http::response<http::string_body>>(status, req->version());
        r->set(http::field::server,       BOOST_BEAST_VERSION_STRING);
        r->set(http::field::content_type, "application/json");
        r->set(http::field::access_control_allow_origin, "*");
        r->keep_alive(false);
        r->body() = body;
        r->prepare_payload();
        http::async_write(stream, *r,
            [self = shared_from_this(), r](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
    };

    if (req->method() == http::verb::get &&
        std::string(req->target()).starts_with("/api/logs")) {
        std::size_t n = 80;
        auto target = std::string(req->target());
        auto pos = target.find("n=");
        if (pos != std::string::npos) {
            try { n = std::stoul(target.substr(pos + 2)); } catch (...) {}
        }
        n = std::min(n, std::size_t{200});
        auto ring = trade_bot::Logger::ring();
        nlohmann::json arr = nlohmann::json::array();
        if (ring) for (const auto& line : ring->recent(n)) arr.push_back(line);
        send_json(http::status::ok, arr.dump());
        return;
    }

    if (req->method() == http::verb::get && req->target() == "/api/state") {
        send_json(http::status::ok, parent.serialize_state());
        return;
    }

    if (req->method() == http::verb::post && req->target() == "/api/dump/start") {
        std::string filename = "dump";
        try {
            auto body = nlohmann::json::parse(req->body());
            filename = body.value("filename", filename);
        } catch (...) {}
        std::string safe;
        for (char c : filename) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')
                safe += c;
        }
        if (safe.empty()) safe = "dump";
        std::string path = "replay/dumps/" + safe + ".ndjson";
        bool ok = parent.recorder_start(path);
        send_json(ok ? http::status::ok : http::status::internal_server_error,
                  ok ? R"({"ok":true})" : R"({"error":"could not open file"})");
        return;
    }

    if (req->method() == http::verb::post && req->target() == "/api/dump/stop") {
        parent.recorder_stop();
        send_json(http::status::ok, R"({"ok":true})");
        return;
    }

    auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req->version());
    res->set(http::field::server,       BOOST_BEAST_VERSION_STRING);
    res->set(http::field::content_type, "text/html; charset=utf-8");
    res->set("Content-Security-Policy",
             "default-src 'self'; script-src 'self' 'unsafe-inline'; "
             "connect-src 'self' ws: wss:; "
             "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
             "font-src https://fonts.gstatic.com; "
             "frame-ancestors 'none'; base-uri 'self'");
    res->set(http::field::x_frame_options, "DENY");
    res->set("X-Content-Type-Options",     "nosniff");
    res->set("Referrer-Policy",            "no-referrer");
    res->keep_alive(req->keep_alive());
    res->body() = kDashboardHtml;
    res->prepare_payload();

    http::async_write(stream, *res,
        [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
            self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        });
}

} // namespace trade_bot
