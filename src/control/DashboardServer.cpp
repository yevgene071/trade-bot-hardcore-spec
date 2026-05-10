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
<html>
<head>
<meta charset="utf-8">
<title>Trade Bot</title>
<style>
:root{--bg:#0f0f0f;--surf:#1a1a1a;--surf2:#242424;--bdr:#2a2a2a;--txt:#e0e0e0;--muted:#666;
  --blue:#3d5afe;--green:#00c853;--red:#f44336;--amber:#ff9800;--cyan:#00bcd4;--purple:#9c27b0;}
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:'Segoe UI',sans-serif;background:var(--bg);color:var(--txt);font-size:14px;}
#header{position:sticky;top:0;z-index:100;background:#111;border-bottom:1px solid var(--bdr);
  padding:8px 20px;display:flex;align-items:center;gap:16px;}
#header h1{font-size:1.05em;color:var(--blue);margin-right:auto;letter-spacing:0.5px;}
.ws-dot{width:8px;height:8px;border-radius:50%;background:var(--red);display:inline-block;vertical-align:middle;}
.ws-dot.live{background:var(--green);}
#stats-bar{display:flex;gap:1px;background:var(--bdr);border-bottom:1px solid var(--bdr);}
.si{flex:1;background:var(--surf);padding:9px 14px;text-align:center;}
.si .lbl{font-size:0.68em;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;}
.si .val{font-size:1.15em;font-weight:600;margin-top:2px;}
.main{padding:14px;display:grid;grid-template-columns:300px 1fr;gap:14px;}
.col{display:flex;flex-direction:column;gap:14px;}
.card{background:var(--surf);border-radius:8px;border:1px solid var(--bdr);overflow:hidden;}
.ch{padding:9px 13px;border-bottom:1px solid var(--bdr);display:flex;align-items:center;gap:7px;
  font-weight:600;font-size:0.88em;}
.cb{padding:11px 13px;}
.dot{width:6px;height:6px;border-radius:50%;flex-shrink:0;}
table{width:100%;border-collapse:collapse;font-size:0.82em;}
th{color:var(--muted);font-weight:500;padding:5px 7px;text-align:left;border-bottom:1px solid var(--bdr);}
td{padding:6px 7px;border-bottom:1px solid #1c1c1c;}
tr:last-child td{border-bottom:none;}
tr:hover td{background:rgba(255,255,255,0.02);}
.kv{display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px solid #1c1c1c;}
.kv:last-child{border-bottom:none;}
.kl{color:var(--muted);font-size:0.83em;}
.kv .kval{font-weight:600;}
.pbar{height:3px;background:var(--bdr);border-radius:2px;margin-top:8px;}
.pbar-fill{height:100%;border-radius:2px;transition:width .4s;}
.mono{font-family:monospace;font-size:0.76em;background:#090909;padding:8px;border-radius:4px;
  max-height:220px;overflow-y:auto;}
.ctog{cursor:pointer;user-select:none;}
.ctog::after{content:' ▲';font-size:.65em;color:var(--muted);}
.ctog.col::after{content:' ▼';}
.green{color:var(--green);} .red{color:var(--red);} .amber{color:var(--amber);}
.cyan{color:var(--cyan);} .muted{color:var(--muted);}
.bdg{display:inline-block;padding:1px 6px;border-radius:10px;font-size:.72em;font-weight:600;}
.bdg-g{background:rgba(0,200,83,.12);color:var(--green);}
.bdg-r{background:rgba(244,67,54,.12);color:var(--red);}
.bdg-a{background:rgba(255,152,0,.12);color:var(--amber);}
@keyframes blink{0%,100%{opacity:1;}50%{opacity:.25;}}
.blink{animation:blink 1.2s infinite;}
#ks-alert{display:none;background:#b71c1c;color:#fff;padding:9px 20px;text-align:center;font-weight:700;}
@media(max-width:860px){.main{grid-template-columns:1fr;}}
</style>
</head>
<body>
<div id="ks-alert">⚠ KILL-SWITCH TRIGGERED ⚠</div>
<div id="header">
  <h1>Trade Bot</h1>
  <span id="version" class="muted" style="font-size:.78em;"></span>
  <span><span id="wsd" class="ws-dot"></span> <span id="wsl" style="font-size:.78em;color:var(--muted);">Disconnected</span></span>
  <span id="upd" style="font-size:.72em;color:var(--muted);"></span>
</div>
<div id="stats-bar">
  <div class="si"><div class="lbl">Equity</div><div class="val" id="se">—</div></div>
  <div class="si"><div class="lbl">Daily PnL</div><div class="val" id="sp">—</div></div>
  <div class="si"><div class="lbl">Win Rate</div><div class="val" id="sw">—</div></div>
  <div class="si"><div class="lbl">Trades</div><div class="val" id="st">—</div></div>
  <div class="si"><div class="lbl">Open</div><div class="val" id="so">—</div></div>
  <div class="si"><div class="lbl">Unrealized</div><div class="val" id="su">—</div></div>
</div>
<div class="main">
<div class="col">
  <div class="card">
    <div class="ch"><span class="dot" style="background:var(--blue)"></span>Account</div>
    <div class="cb">
      <div class="kv"><span class="kl">Equity</span><span class="kval" id="eq">$0.00</span></div>
      <div class="kv"><span class="kl">Free Balance</span><span class="kval" id="fb">$0.00</span></div>
      <div class="kv"><span class="kl">Realized PnL (day)</span><span class="kval" id="pt">+0.00</span></div>
      <div class="kv"><span class="kl">Unrealized PnL</span><span class="kval" id="pu">+0.00</span></div>
      <div class="kv"><span class="kl">Starting Equity</span><span class="kval muted" id="seq">$0.00</span></div>
      <div class="pbar"><div class="pbar-fill" id="pbf" style="width:0%;background:var(--green)"></div></div>
    </div>
  </div>
  <div class="card">
    <div class="ch"><span class="dot" style="background:var(--green)"></span>Signal Counters</div>
    <div class="cb" id="signal-list"></div>
  </div>
  <div class="card">
    <div class="ch"><span class="dot" style="background:var(--purple)"></span>Strategy Performance</div>
    <div class="cb" style="padding:0;">
      <table><thead><tr><th>Strategy</th><th>W / L</th><th>PnL $</th></tr></thead>
      <tbody id="strat-stats"></tbody></table>
    </div>
  </div>
  <div class="card">
    <div class="ch"><span class="dot" style="background:var(--cyan)"></span>Universe (<span id="uni-n">0</span>)</div>
    <div style="padding:0;">
      <table><thead><tr><th>Ticker</th><th style="text-align:right">Mark</th><th>Strategies</th></tr></thead>
      <tbody id="universe-list"></tbody></table>
    </div>
  </div>
  <div class="card">
    <div class="ch"><span class="dot" style="background:var(--amber)"></span>Dump Recorder</div>
    <div class="cb">
      <div class="kv"><span class="kl">Status</span><span id="recs" class="kval muted">idle</span></div>
      <div id="rec-pr" style="display:none;" class="kv"><span class="kl">File</span>
        <span id="recp" class="kval muted" style="font-size:.78em;word-break:break-all;text-align:right;max-width:170px;"></span></div>
      <div style="display:flex;gap:7px;margin-top:9px;">
        <input id="rec-fn" type="text" placeholder="session1"
          style="flex:1;background:var(--surf2);color:var(--txt);border:1px solid var(--bdr);border-radius:4px;padding:4px 7px;font-size:.82em;">
        <button id="rec-btn" onclick="toggleRecord()"
          style="background:var(--amber);color:#000;border:none;border-radius:4px;padding:4px 13px;cursor:pointer;font-weight:600;font-size:.82em;">Start</button>
      </div>
    </div>
  </div>
</div>
<div class="col">
  <div class="card">
    <div class="ch">
      <span class="dot blink" id="pos-dot" style="background:var(--red);display:none;"></span>
      <span class="dot" id="pos-dot-idle" style="background:var(--muted)"></span>
      Open Positions
      <span id="pos-cnt" class="bdg bdg-a" style="display:none;margin-left:3px;"></span>
    </div>
    <div style="overflow-x:auto;">
      <table id="pos-tbl">
        <thead><tr><th>Ticker</th><th>Side</th><th>Size</th><th>Entry</th><th>Stop</th><th>TP</th><th>uPnL</th><th>Risk $</th></tr></thead>
        <tbody></tbody>
      </table>
      <div id="no-pos" style="padding:14px;color:var(--muted);font-size:.82em;text-align:center;">No open positions</div>
    </div>
  </div>
  <div class="card">
    <div class="ch"><span class="dot" style="background:var(--blue)"></span>
      Trade Journal <span id="jcnt" class="muted" style="font-size:.78em;margin-left:4px;"></span></div>
    <div style="overflow-x:auto;">
      <table id="jrn-tbl">
        <thead><tr><th>Ticker</th><th>Strat</th><th>Side</th><th>Entry</th><th>Exit</th><th>Size</th><th>PnL $</th><th>PnL %</th><th>Reason</th></tr></thead>
        <tbody></tbody>
      </table>
      <div id="no-jrn" style="padding:14px;color:var(--muted);font-size:.82em;text-align:center;display:none;">No closed trades yet</div>
    </div>
  </div>
  <div class="card">
    <div class="ch ctog" id="sig-tog" onclick="tog('sig-body','sig-tog')">
      <span class="dot" style="background:var(--green)"></span>Signal Feed
      <span id="sig-cnt" class="muted" style="font-size:.78em;margin-left:4px;"></span>
      <span style="margin-left:auto;">
        <select id="sig-flt" style="background:var(--surf2);color:var(--txt);border:1px solid var(--bdr);border-radius:4px;padding:2px 5px;font-size:.78em;" onclick="event.stopPropagation()">
          <option value="">All</option>
        </select>
      </span>
    </div>
    <div id="sig-body"><div class="mono" id="sig-feed"></div></div>
  </div>
  <div class="card">
    <div class="ch ctog" id="log-tog" onclick="tog('log-body','log-tog')">
      <span class="dot" style="background:var(--purple)"></span>Live Log
      <span id="log-cnt" class="muted" style="font-size:.78em;margin-left:4px;"></span>
    </div>
    <div id="log-body"><div class="mono" id="log-panel">Connecting…</div></div>
  </div>
</div>
</div>
<script>
function $(id){return document.getElementById(id);}
function ce(t,c,tx){const e=document.createElement(t);if(c)e.className=c;if(tx!==undefined)e.textContent=tx;return e;}
function pc(v){return v>=0?'green':'red';}
function fp(v){return(v>=0?'+':'')+Number(v).toFixed(2);}
function fpct(v){return(v>=0?'+':'')+Number(v).toFixed(2)+'%';}
function tog(bid,tid){const b=$(bid),t=$(tid);const h=b.style.display==='none';b.style.display=h?'':'none';t.classList.toggle('col',!h);}

function updateUI(s){
  const acc=s.account||{},jrn=s.recent_journal||[],trades=s.open_trades||[];
  const eq=Number(acc.equity_usd||0),pt=Number(acc.realized_pnl_today_usd||0);
  const seq=Number(acc.starting_equity_usd||eq),upnl=Number(acc.unrealized_pnl_usd||0);
  const pct=seq>0?pt/seq*100:0;
  const wins=jrn.filter(e=>Number(e.pnl_usd)>0).length;
  const wr=jrn.length>0?(wins/jrn.length*100).toFixed(0)+'%':'—';
  const open=trades.filter(t=>t.avg_entry_price>0).length;

  // Stats bar
  $('se').textContent='$'+eq.toFixed(2);
  const spEl=$('sp');spEl.textContent=fp(pt)+' ('+fpct(pct)+')';spEl.className='val '+pc(pt);
  $('sw').textContent=wr; $('sw').className='val '+(jrn.length&&wins/jrn.length>=.5?'green':'red');
  $('st').textContent=jrn.length;
  $('so').textContent=open; $('so').className='val '+(open?'amber':'muted');
  const suEl=$('su');suEl.textContent=fp(upnl);suEl.className='val '+pc(upnl);

  // Account card
  $('version').textContent=s.version?'v'+s.version:'';
  $('eq').textContent='$'+eq.toFixed(2);
  $('fb').textContent='$'+Number(acc.free_balance_usd||0).toFixed(2);
  const ptEl=$('pt');ptEl.textContent=fp(pt);ptEl.className='kval '+pc(pt);
  const puEl=$('pu');puEl.textContent=fp(upnl);puEl.className='kval '+pc(upnl);
  $('seq').textContent='$'+seq.toFixed(2);
  const bpct=Math.min(Math.abs(pct),5)/5*100;
  $('pbf').style.width=bpct+'%';$('pbf').style.background=pt>=0?'var(--green)':'var(--red)';

  // Kill switch
  $('ks-alert').style.display=s.kill_switch_active?'block':'none';

  // Signal counters
  const sl=$('signal-list');sl.replaceChildren();
  for(const[k,v] of Object.entries(s.signal_counts||{})){
    const r=ce('div','kv');r.appendChild(ce('span','kl',k+':'));r.appendChild(ce('span','kval',String(v)));sl.appendChild(r);
  }

  // Mark price map
  const mk={};for(const r of(s.universe||[]))mk[r.ticker]=r.mark_price;

  // Positions
  const posBody=document.querySelector('#pos-tbl tbody');posBody.replaceChildren();
  const openTrades=trades.filter(t=>t.avg_entry_price>0);
  $('no-pos').style.display=openTrades.length?'none':'';
  $('pos-dot').style.display=openTrades.length?'':'none';
  $('pos-dot-idle').style.display=openTrades.length?'none':'';
  const pcEl=$('pos-cnt');
  pcEl.style.display=openTrades.length?'':'none';if(openTrades.length)pcEl.textContent=openTrades.length;

  for(const t of openTrades){
    const tr=document.createElement('tr');
    const u=Number(t.unrealized_pnl||0),isBuy=t.plan.side===1;
    const entry=Number(t.avg_entry_price||0),stop=Number(t.plan.stop_price||0),tp=Number(t.plan.tp1_price||0);
    const size=Number(t.executed_size||0),risk=stop>0?Math.abs(entry-stop)*size:0;
    [[t.plan.ticker,'cyan'],[isBuy?'LONG':'SHORT',isBuy?'green':'red'],
     [size.toFixed(4)],[entry.toFixed(4)],
     [stop>0?stop.toFixed(4):'—','red'],[tp>0?tp.toFixed(4):'—','green'],
     [fp(u),pc(u)],[risk>0?'$'+risk.toFixed(2):'—','amber']
    ].forEach(([txt,cls])=>{const td=ce('td',cls||'',txt);tr.appendChild(td);});
    posBody.appendChild(tr);
  }

  // Journal + strategy stats
  const jBody=document.querySelector('#jrn-tbl tbody');jBody.replaceChildren();
  $('jcnt').textContent=jrn.length?'('+jrn.length+' recent)':'';
  $('no-jrn').style.display=jrn.length?'none':'';
  const sm={};
  for(const e of jrn){
    const pnl=Number(e.pnl_usd||0),isBuy=e.plan?.side===1;
    const ep=Number(e.plan?.entry_price||0),xp=Number(e.exit_price||0);
    const sz=Number(e.plan?.size_coin||0);
    const pnlPct=ep>0&&sz>0?pnl/(ep*sz)*100:0;
    const sn=e.plan?.strategy_name||'—';
    if(!sm[sn])sm[sn]={w:0,l:0,pnl:0};
    pnl>0?sm[sn].w++:sm[sn].l++;sm[sn].pnl+=pnl;
    const tr=document.createElement('tr');
    [[e.plan?.ticker||'—','cyan'],[sn,'muted'],[isBuy?'L':'S',isBuy?'green':'red'],
     [ep>0?ep.toFixed(4):'—'],[xp>0?xp.toFixed(4):'—'],[sz.toFixed(4)],
     [fp(pnl),pc(pnl)],[fpct(pnlPct),pc(pnl)],[e.cause_of_exit||'—','muted']
    ].forEach(([txt,cls])=>{const td=ce('td',cls||'',txt);tr.appendChild(td);});
    jBody.appendChild(tr);
  }

  // Strategy stats
  const sb=$('strat-stats');sb.replaceChildren();
  const entries=Object.entries(sm);
  if(!entries.length){
    const tr=document.createElement('tr');const td=ce('td','muted','No closed trades yet');td.colSpan=3;tr.appendChild(td);sb.appendChild(tr);
  }
  for(const[n,st] of entries){
    const tot=st.w+st.l,wr=tot>0?(st.w/tot*100).toFixed(0)+'%':'—';
    const tr=document.createElement('tr');
    [[n],[st.w+'W / '+st.l+'L ('+wr+')',st.w>=st.l?'green':'red'],[fp(st.pnl),pc(st.pnl)]
    ].forEach(([txt,cls])=>{const td=ce('td',cls||'',txt);tr.appendChild(td);});
    sb.appendChild(tr);
  }

  // Universe
  const ub=$('universe-list');ub.replaceChildren();
  $('uni-n').textContent=(s.universe||[]).length;
  for(const r of(s.universe||[])){
    const tr=document.createElement('tr');
    const tk=ce('td','','');tk.textContent=r.ticker+(r.boosted?' ★':'');if(r.boosted)tk.style.color='#ffeb3b';
    const mk2=ce('td','cyan',r.mark_price?'$'+Number(r.mark_price).toFixed(2):'—');mk2.style.textAlign='right';
    const st=ce('td','muted',(r.strategies||[]).join(', ')||'none');
    tr.append(tk,mk2,st);ub.appendChild(tr);
  }

  // Signal feed
  const fsel=$('sig-flt');
  const known=new Set([...fsel.options].map(o=>o.value).filter(v=>v));
  for(const r of(s.universe||[])){if(!known.has(r.ticker)){const o=ce('option','',r.ticker);o.value=r.ticker;fsel.appendChild(o);}}
  const sigs=s.recent_signals||[];const fv=fsel.value;
  const fil=fv?sigs.filter(sg=>sg.ticker===fv):sigs;
  $('sig-cnt').textContent=sigs.length?'('+sigs.length+')':'';
  const sf=$('sig-feed');
  const atB=sf.scrollTop>=sf.scrollHeight-sf.clientHeight-20;
  sf.replaceChildren();
  for(const sg of [...fil].reverse()){
    const d=ce('div');d.style.marginBottom='2px';
    const ts=ce('span');ts.style.color='#444';ts.textContent=sg.time_str+' ';
    const tk=ce('span');tk.style.color='var(--cyan)';tk.textContent=sg.ticker+' ';
    const kd=ce('span');kd.style.color='var(--green)';kd.textContent=sg.kind+' ';
    const pr=ce('span');pr.textContent='@'+Number(sg.price||0).toFixed(2)+' ';
    const cf=ce('span');cf.style.color='var(--amber)';cf.textContent='['+Number(sg.confidence||0).toFixed(2)+']';
    d.append(ts,tk,kd,pr,cf);sf.appendChild(d);
  }
  if(atB||!fil.length)sf.scrollTop=sf.scrollHeight;

  // Recorder
  const rec=s.dump_recorder||{};
  _rec=!!rec.active;
  $('recs').textContent=rec.active?'● RECORDING':'idle';
  $('recs').className='kval '+(rec.active?'red blink':'muted');
  $('rec-pr').style.display=rec.active?'':'none';
  if(rec.active)$('recp').textContent=rec.path||'';
  $('rec-btn').textContent=rec.active?'Stop':'Start';
  $('rec-btn').style.background=rec.active?'var(--red)':'var(--amber)';

  $('upd').textContent=new Date().toLocaleTimeString();
}

let _rec=false;
function toggleRecord(){
  if(_rec){fetch('/api/dump/stop',{method:'POST'}).catch(console.error);}
  else{const n=$('rec-fn').value.trim()||'dump';
    fetch('/api/dump/start',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({filename:n})}).catch(console.error);}
  _rec=!_rec;
}

// WS
let bk=500;
function connect(){
  const ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onopen=()=>{bk=500;$('wsd').className='ws-dot live';$('wsl').textContent='Live';$('wsl').style.color='var(--green)';};
  ws.onmessage=ev=>{try{updateUI(JSON.parse(ev.data));}catch(e){console.error(e);}};
  ws.onclose=()=>{$('wsd').className='ws-dot';$('wsl').textContent='Reconnecting…';$('wsl').style.color='var(--amber)';
    bk=Math.min(bk*2,30000);setTimeout(connect,bk);};
  ws.onerror=()=>ws.close();
}
connect();

// Log
const LC={'error':'#f44336','critical':'#ff1744','warning':'#ff9800','warn':'#ff9800','info':'#80cbc4','debug':'#888','trace':'#555'};
let _ll=false;
const lp=$('log-panel');
lp.addEventListener('scroll',()=>{_ll=lp.scrollTop<lp.scrollHeight-lp.clientHeight-20;});
function fetchLogs(){
  fetch('/api/logs?n=80').then(r=>r.json()).then(lines=>{
    lp.replaceChildren();
    for(const line of lines){const s=ce('span');const m=line.match(/\[(error|critical|warning|warn|info|debug|trace)\]/i);
      s.style.color=m?(LC[m[1].toLowerCase()]||'#e0e0e0'):'#e0e0e0';s.textContent=line+'\n';lp.appendChild(s);}
    $('log-cnt').textContent=lines.length+' lines';
    if(!_ll)lp.scrollTop=lp.scrollHeight;
  }).catch(()=>{});
}
fetchLogs();setInterval(fetchLogs,2000);
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
        payload = serialize_state_locked_();
        snap.assign(sessions_.begin(), sessions_.end());
    }
    LOG_TRACE("Dashboard update: {} bytes, {} sessions", payload.size(), snap.size());
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
             "style-src 'self' 'unsafe-inline'; "
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
