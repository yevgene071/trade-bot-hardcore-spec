"""
Dashboard Audit — Backend ↔ Frontend data flow & gap analysis  (code-verified 2026-05-15)
Output: reports/dashboard_audit.png
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
from matplotlib.gridspec import GridSpec

GREEN  = '#27ae60'
YELLOW = '#e67e22'
RED    = '#e74c3c'
BLUE   = '#2980b9'
GRAY   = '#7f8c8d'
BG     = '#0f1923'
BG2    = '#16213e'
WHITE  = '#ecf0f1'
SILVER = '#bdc3c7'
DIM    = '#4a6fa5'

# ── status icons ────────────────────────────────────────────────
OK   = ('✓', GREEN)
WARN = ('~', YELLOW)
BUG  = ('✗', RED)
SKIP = ('○', GRAY)

# ── Backend → Store → Component mapping ─────────────────────────
# (backend_field, status_tuple, store_field, consumer)
FLOW = [
    # ── Account ──────────────────────────────────────────────────
    ('account.equity_usd',              OK,   'equity',            'Header, CommandCenter'),
    ('account.realized_pnl_today_usd',  OK,   'sessionPnL',        'Header, CommandCenter'),
    ('account.unrealized_pnl_usd',      SKIP, '—',                 '(not shown)'),
    ('account.free_balance_usd',        SKIP, '—',                 '(not shown)'),
    ('account.starting_equity_usd',     OK,   'sessionPnLPct',     'Header (pct calc)'),
    # ── Risk ─────────────────────────────────────────────────────
    ('risk.margin_used_pct',            OK,   'marginUsedPct',     'Header'),
    ('risk.current_drawdown_pct',       OK,   'dailyDrawdownPct',  'Header'),
    ('risk.exposure_pct',               SKIP, '—',                 '(not shown)'),
    ('risk.total_trades_today',         SKIP, '—',                 '(not shown)'),
    ('risk.consecutive_losses',         SKIP, '—',                 '(not shown)'),
    ('risk.max_positions',              SKIP, '—',                 '(not shown)'),
    ('risk.daily_pnl_pct',              SKIP, '—',                 '(not shown)'),
    # ── MetaScalp ────────────────────────────────────────────────
    ('metascalp.latency_ms',            OK,   'latency',           'Sidebar'),
    ('metascalp.connection_name',       SKIP, '—',                 '(not shown)'),
    # ── Kill switch / misc ───────────────────────────────────────
    ('kill_switch_active',              OK,   'killSwitchActive',  'Header toggle, Sidebar dot'),
    ('selected_ticker',                 OK,   'activeTicker',      'Chart, ticker buttons'),
    ('server_time_unix',                SKIP, '—',                 '(not shown)'),
    ('version',                         SKIP, '—',                 '(not shown)'),
    # ── Trades / Journal ─────────────────────────────────────────
    ('open_trades',                     BUG,  'trades (broken)',   'Portfolio (BROKEN: plan.* → undef)'),
    ('recent_journal',                  WARN, 'journalEntries',    'Journal tab (side=int bug)'),
    # ── Signals ──────────────────────────────────────────────────
    ('recent_signals',                  WARN, 'signals',           'Market right panel (side=always BUY)'),
    ('signal_counts',                   SKIP, '—',                 '(not consumed)'),
    # ── Universe / Strategies ─────────────────────────────────────
    ('universe',                        SKIP, '—',                 '(not consumed — mark_prices lost)'),
    ('strategy_states',                 OK,   'strategyStates',    'Strategies tab, CC heatmap'),
    ('strategy_stats',                  SKIP, '—',                 '(not consumed — no stats tab)'),
    # ── Funding / Recorder ───────────────────────────────────────
    ('funding_info',                    SKIP, '—',                 '(not consumed — no UI)'),
    ('recorder_active / recorder_path', SKIP, '—',                 '(not consumed — no UI controls)'),
    # ── Chart history ────────────────────────────────────────────
    ('chart_history.spread_bps',        OK,   'spreadHistory',     'Market analytics mini-chart'),
    ('chart_history.tape_aggression',   OK,   'aggressionHistory', 'Market analytics mini-chart'),
    ('chart_history.buy/sell_vol_5s',   OK,   'volumeHistory',     'Market analytics mini-chart'),
    ('chart_history.volatility_1min',   SKIP, '—',                 '(not consumed — mini-chart missing)'),
    ('chart_history.leader_correlation',SKIP, '—',                 '(not consumed)'),
    ('chart_history.leader_change_1s',  SKIP, '—',                 '(not consumed)'),
    ('chart_history.best_bid/ask',      SKIP, '—',                 '(not consumed)'),
    # ── Orderbook ────────────────────────────────────────────────
    ('bids_top20 / asks_top20',         OK,   'orderbook.bids/asks','Ladder ✓'),
    ('ob_mid / ob_spread_bps',          OK,   'orderbook.mid/spread','Ladder spread display ✓'),
    ('ob_imbalance',                    OK,   'orderbook.imbalance', 'Ladder imbalance ✓'),
    # ── History ──────────────────────────────────────────────────
    ('equity_history',                  OK,   'equityHistory',     'CommandCenter equity curve ✓'),
    # ── strategy_states extras ───────────────────────────────────
    ('strategy_states.signals_last_60s',SKIP, '—',                 '(not shown in StrategiesView)'),
]

# ── Bugs & Stubs ─────────────────────────────────────────────────
BUGS = [
    ('BUG-1', RED,
     'open_trades: nested plan.{ticker,side(int),...}\n'
     'store assigns raw → t.ticker=undefined\n'
     '→ Portfolio tab always shows nothing'),
    ('BUG-2', RED,
     'recent_journal.side = int (0/1 enum)\n'
     'displayed as raw int, not "LONG"/"SHORT"\n'
     '→ Journal badge always broken'),
    ('BUG-3', RED,
     'Signal side: conf>0 ? "BUY" : "SELL"\n'
     'confidence ∈ [0,1] → always "BUY"\n'
     '→ all sell signals show as BUY'),
    ('BUG-4', RED,
     '/api/command → returns {"ok":true} always\n'
     'No command parsing in DashboardServer.cpp\n'
     '→ all text commands are no-ops'),
    ('BUG-5', YELLOW,
     'CommandCenter: Margin="4.2%" & Draw="-1.1%"\n'
     'hardcoded strings in StatCard children\n'
     '→ ignores real risk data from store'),
]

STUBS = [
    ('STUB-1', YELLOW,
     'TradingPanel order submit\n'
     '→ /api/command (stub)\n'
     '→ order never reaches bot'),
    ('STUB-2', YELLOW,
     'Strategy enable/disable buttons\n'
     '→ /api/command (stub)\n'
     '→ no effect on TickerController'),
    ('STUB-3', YELLOW,
     'Portfolio "Market Close" button\n'
     '→ no onClick handler wired\n'
     '→ completely inert'),
    ('STUB-4', YELLOW,
     'DumpRecorder: /api/dump/start + stop\n'
     'endpoints exist in DashboardServer\n'
     '→ zero UI controls exposed'),
]

# ── API endpoints ─────────────────────────────────────────────────
APIS = [
    ('GET /api/logs',          OK,   'LogsView — polled on open, manual refresh'),
    ('GET /api/state',         WARN, 'Exists but not used — WS replaces it'),
    ('POST /api/ticker/select',OK,   'apiSelectTicker() ✓'),
    ('POST /api/killswitch/toggle', OK, 'apiToggleKillSwitch() ✓'),
    ('POST /api/dump/start',   SKIP, 'Backend ready — NO UI controls'),
    ('POST /api/dump/stop',    SKIP, 'Backend ready — NO UI controls'),
    ('POST /api/command',      BUG,  'STUB: always returns ok=true, ignores body'),
    ('WS /ws',                 OK,   'useWsTransport, 2s reconnect, applyServerState()'),
]

# ── Feature completeness ──────────────────────────────────────────
FEATURES = [
    ('WS Connection',     95, GREEN),
    ('Kill Switch',       90, GREEN),
    ('Equity / PnL',      85, GREEN),
    ('Ladder / OB',       92, GREEN),
    ('Strategies tab',    85, GREEN),
    ('Logs tab',          88, GREEN),
    ('Market chart',      70, YELLOW),
    ('Signals feed',      60, YELLOW),
    ('Analytics charts',  65, YELLOW),
    ('Journal tab',       60, YELLOW),
    ('Command Center',    38, RED),
    ('Portfolio tab',     12, RED),
    ('TradingPanel',      15, RED),
    ('Strat enable/dis',   5, RED),
    ('Recorder UI',        0, RED),
    ('Funding info UI',    0, RED),
    ('Strategy Stats UI',  0, RED),
    ('Universe marks UI',  0, RED),
]

# ══════════════════════════════════════════════════════════════════
fig = plt.figure(figsize=(28, 17), facecolor=BG)
fig.suptitle(
    'Dashboard Audit — Backend ↔ Frontend Data Flow & Gap Analysis  (code-verified 2026-05-15)',
    fontsize=13, color=WHITE, fontweight='bold', y=0.998)

gs = GridSpec(2, 3, figure=fig,
              left=0.01, right=0.995, top=0.976, bottom=0.003,
              width_ratios=[1.15, 1.0, 0.85],
              height_ratios=[1.0, 0.22],
              wspace=0.025, hspace=0.04)

ax_flow  = fig.add_subplot(gs[0, 0])
ax_bugs  = fig.add_subplot(gs[0, 1])
ax_api   = fig.add_subplot(gs[0, 2])
ax_bars  = fig.add_subplot(gs[1, :])

for ax in [ax_flow, ax_bugs, ax_api, ax_bars]:
    ax.set_facecolor(BG2)
    ax.axis('off')

STATUS_COLOR = {id(OK): GREEN, id(WARN): YELLOW, id(BUG): RED, id(SKIP): GRAY}

# ─────────────────────────────────────────────────────────────────
# PANEL 1: Backend → Store → Component flow
# ─────────────────────────────────────────────────────────────────
ax_flow.text(0.5, 0.995, 'Backend → Store → Consumer  (field-level)',
             color=WHITE, fontsize=9.0, fontweight='bold',
             ha='center', va='top', transform=ax_flow.transAxes)

# Legend row
for xi, (icon, clr), lbl in [
    (0.02, OK,   'OK'),
    (0.18, WARN, 'Partial/Bug'),
    (0.38, BUG,  'Broken'),
    (0.56, SKIP, 'Not consumed'),
]:
    ax_flow.add_patch(FancyBboxPatch((xi, 0.970), 0.03, 0.018,
        boxstyle='round,pad=0.002', facecolor=clr, alpha=0.85,
        transform=ax_flow.transAxes, clip_on=False))
    ax_flow.text(xi+0.035, 0.979, lbl, transform=ax_flow.transAxes,
                 fontsize=5.5, color=clr, va='center')

ax_flow.plot([0.0, 1.0], [0.962, 0.962], color='#1e3a5f', lw=0.6,
             transform=ax_flow.transAxes)

# Column headers
for xi, lbl in [(0.0, 'Backend field'), (0.44, 'Store key'), (0.68, 'Consumer')]:
    ax_flow.text(xi, 0.955, lbl, transform=ax_flow.transAxes,
                 fontsize=6.0, color=SILVER, va='top', fontweight='bold')

y = 0.940
row_h = 0.023
for backend, status, store, consumer in FLOW:
    sc = STATUS_COLOR[id(status)]
    icon = status[0]
    # colored status square
    ax_flow.add_patch(FancyBboxPatch((0.0, y-0.013), 0.018, 0.018,
        boxstyle='round,pad=0.001', facecolor=sc, alpha=0.80,
        transform=ax_flow.transAxes, clip_on=False))
    ax_flow.text(0.009, y-0.004, icon, transform=ax_flow.transAxes,
                 fontsize=5.2, color='white', va='center', ha='center', fontweight='bold')
    ax_flow.text(0.022, y-0.004, backend, transform=ax_flow.transAxes,
                 fontsize=5.6, color=sc if sc != GRAY else SILVER, va='center')
    ax_flow.text(0.44, y-0.004, store, transform=ax_flow.transAxes,
                 fontsize=5.5, color=WHITE if sc == GREEN else (YELLOW if sc == YELLOW else (RED if sc == RED else GRAY)),
                 va='center')
    ax_flow.text(0.68, y-0.004, consumer, transform=ax_flow.transAxes,
                 fontsize=5.2, color=SILVER, va='center')
    y -= row_h

# ─────────────────────────────────────────────────────────────────
# PANEL 2: Bugs + Stubs
# ─────────────────────────────────────────────────────────────────
ax_bugs.text(0.5, 0.995, 'Bugs & Stubs',
             color=WHITE, fontsize=9.0, fontweight='bold',
             ha='center', va='top', transform=ax_bugs.transAxes)

def draw_issue_block(ax, items, y_start, section_label, section_color):
    y = y_start
    ax.add_patch(FancyBboxPatch((0.01, y-0.020), 0.98, 0.028,
        boxstyle='round,pad=0.003', facecolor=section_color, alpha=0.25,
        transform=ax.transAxes, clip_on=False))
    ax.text(0.5, y-0.006, section_label, transform=ax.transAxes,
            fontsize=7.5, color=section_color, fontweight='bold',
            ha='center', va='center')
    y -= 0.030
    for tag, color, desc in items:
        lines = desc.split('\n')
        nl = len(lines)
        box_h = 0.020 + nl * 0.021
        ax.add_patch(FancyBboxPatch((0.01, y-box_h+0.004), 0.98, box_h,
            boxstyle='round,pad=0.004', facecolor=color, alpha=0.12,
            edgecolor=color, linewidth=0.4,
            transform=ax.transAxes, clip_on=False))
        ax.text(0.03, y-0.000, tag, transform=ax.transAxes,
                fontsize=6.5, color=color, fontweight='bold', va='top')
        for i, line in enumerate(lines):
            ax.text(0.14, y - i*0.021, line, transform=ax.transAxes,
                    fontsize=6.0, color=WHITE if color == RED else SILVER,
                    va='top')
        y -= box_h + 0.010
    return y

y = 0.965
y = draw_issue_block(ax_bugs, BUGS,  y, '── BUGs: Data Mapping Errors ──', RED)
y -= 0.012
y = draw_issue_block(ax_bugs, STUBS, y, '── STUBs: UI wired but no-op ──', YELLOW)

# ─────────────────────────────────────────────────────────────────
# PANEL 3: API endpoints + Not-consumed summary
# ─────────────────────────────────────────────────────────────────
ax_api.text(0.5, 0.995, 'API Endpoints',
            color=WHITE, fontsize=9.0, fontweight='bold',
            ha='center', va='top', transform=ax_api.transAxes)

y = 0.965
for endpoint, status, desc in APIS:
    sc = STATUS_COLOR[id(status)]
    icon = status[0]
    ax_api.add_patch(FancyBboxPatch((0.01, y-0.018), 0.022, 0.020,
        boxstyle='round,pad=0.001', facecolor=sc, alpha=0.80,
        transform=ax_api.transAxes, clip_on=False))
    ax_api.text(0.021, y-0.008, icon, transform=ax_api.transAxes,
                fontsize=5.5, color='white', va='center', ha='center', fontweight='bold')
    ax_api.text(0.040, y-0.008, endpoint, transform=ax_api.transAxes,
                fontsize=6.0, color=sc, fontweight='bold', va='center')
    ax_api.text(0.040, y-0.022, desc, transform=ax_api.transAxes,
                fontsize=5.4, color=SILVER, va='center')
    y -= 0.046

# Divider + Not consumed summary
ax_api.plot([0.01, 0.99], [y+0.01, y+0.01], color='#1e3a5f', lw=0.6,
             transform=ax_api.transAxes)
y -= 0.005
ax_api.text(0.5, y, '── Backend fields with no UI ──',
            transform=ax_api.transAxes, fontsize=7.0, color=GRAY,
            fontweight='bold', ha='center', va='top')
y -= 0.030

not_consumed = [
    'signal_counts              (signal type totals, never displayed)',
    'universe.mark_price        (not distributed to tickerPrices)',
    'strategy_stats             (win/loss/pnl per strategy — no tab)',
    'funding_info               (rate + next_ts — no UI section)',
    'recorder_active/path       (dump state — no UI controls)',
    'server_time_unix           (never shown)',
    'account.unrealized_pnl_usd (not shown in header)',
    'account.free_balance_usd   (not shown)',
    'risk.exposure_pct          (not shown)',
    'risk.total_trades_today    (not shown)',
    'risk.consecutive_losses    (not shown)',
    'risk.daily_pnl_pct         (not shown)',
    'risk.max_positions         (not shown)',
    'metascalp.connection_name  (not shown in sidebar)',
    'chart_history.volatility   (mini-chart missing)',
    'chart_history.leader_corr  (no chart)',
    'chart_history.leader_Δ1s   (no chart)',
    'strategy_states.signals60s (StrategiesView ignores it)',
]
for item in not_consumed:
    ax_api.text(0.02, y, item, transform=ax_api.transAxes,
                fontsize=5.4, color=GRAY, va='top')
    y -= 0.022

# ─────────────────────────────────────────────────────────────────
# BOTTOM: Feature completeness bars
# ─────────────────────────────────────────────────────────────────
n = len(FEATURES)
bw = 1.0 / n

for i, (name, pct, color) in enumerate(FEATURES):
    x0 = i * bw
    xc = x0 + bw / 2
    ax_bars.add_patch(FancyBboxPatch(
        (x0+0.002, 0.28), bw-0.004, 0.57,
        boxstyle='round,pad=0.01', facecolor='#1e3a5f',
        transform=ax_bars.transAxes, clip_on=False))
    fill_w = (bw-0.004) * pct / 100.0
    if fill_w > 0.002:
        ax_bars.add_patch(FancyBboxPatch(
            (x0+0.002, 0.28), fill_w, 0.57,
            boxstyle='round,pad=0.01', facecolor=color, alpha=0.85,
            transform=ax_bars.transAxes, clip_on=False))
    ax_bars.text(xc, 0.64, f'{pct}%', transform=ax_bars.transAxes,
                 fontsize=5.8, color='white', ha='center', va='center', fontweight='bold')
    ax_bars.text(xc, 0.16, name, transform=ax_bars.transAxes,
                 fontsize=5.1, color=SILVER, ha='center', va='center', rotation=28)

ax_bars.text(0.5, 0.03, 'Dashboard Feature Completeness  (% — code-verified)',
             transform=ax_bars.transAxes, fontsize=7.5, color=GRAY, ha='center')

out = '/home/yevge/trade-bot-hardcore-spec/reports/dashboard_audit.png'
plt.savefig(out, dpi=160, bbox_inches='tight', facecolor=BG)
print(f'Saved: {out}')
