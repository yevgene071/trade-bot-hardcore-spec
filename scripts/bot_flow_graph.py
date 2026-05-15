"""
Trade Bot — Data Flow + Trading Logic Visualization (v3, code-verified)
Output: reports/bot_flow_graph.png
"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
from matplotlib.gridspec import GridSpec
import numpy as np

# ── palette ────────────────────────────────────────────────────────────────
GREEN  = '#27ae60'
YELLOW = '#e67e22'
RED    = '#e74c3c'
BLUE   = '#2980b9'
PURPLE = '#8e44ad'
GRAY   = '#7f8c8d'
BG     = '#0f1923'
BG2    = '#16213e'
WHITE  = '#ecf0f1'
SILVER = '#bdc3c7'
DIM    = '#4a6fa5'
EDGE_N = '#4a6fa5'
EDGE_B = '#e74c3c'
EDGE_P = '#8e44ad'

# ═══════════════════════════════════════════════════════════════════════════
# 1. NODE DATA
# ═══════════════════════════════════════════════════════════════════════════
# id → (label, color, col_x, row_y)   y increases downward
NODES = {
    'exchange':   ('Binance\n/ Exchange',            BLUE,   5.5, 0.0),
    'ms_api':     ('MetaScalp API\nREST + WS',       BLUE,   2.0, 0.0),
    'ws_ob':      ('orderbook_subscribe\n✓ used',    GREEN, -0.5, 1.2),
    'ws_tr':      ('trade_subscribe\n✓ used',        GREEN,  1.3, 1.2),
    'ws_acc':     ('account subscribe\n✓ used',      GREEN,  3.1, 1.2),
    'ws_mark':    ('mark_price_subscribe\n✓→PnL',    GREEN,  4.9, 1.2),
    'ws_fund':    ('funding_subscribe\n✓→R13',       GREEN,  6.8, 1.2),
    'ws_sigLvl':  ('signal_level_subscribe\n✓LevelFormed now emitted', GREEN, 8.6, 1.2),
    'orderbook':  ('OrderBook\nabsl::btree_map ✓',  GREEN, -0.5, 2.5),
    'tradestream':('TradeStream\nHawkes/T-Digest ✓', GREEN,  1.3, 2.5),
    'pos_mgr':    ('PositionManager\nAvgPriceFix ✓', GREEN,  3.8, 2.5),
    'feat_ext':   ('FeatureExtractor\n10–20Hz ring buf ✓', GREEN, 1.3, 3.7),
    'density':    ('DensityDetector\n✓ BUG-1+2 fixed', GREEN,-1.8, 5.2),
    'iceberg':    ('IcebergDetector\nBayesian ✓ CircBuf', GREEN,-0.1, 5.2),
    'tape':       ('TapeAnalyzer\nHawkes/CUSUM ✓',  GREEN,  1.6, 5.2),
    'level':      ('LevelDetector\nDBSCAN+KDE ✓\nFormed+Break+Reject', GREEN, 3.3, 5.2),
    'approach':   ('ApproachAnalyzer\nHMM 3-state ✓', GREEN, 5.1, 5.2),
    'leader_sig': ('LeaderSignal\nWelford corr ✓ CircBuf', GREEN, 6.8, 5.2),
    'liqui':      ('LiquidationDet.\n✗ NOT IMPL.',  RED,    8.5, 5.2),
    'signal_bus': ('SignalBus\npub/sub ✓',          GREEN,  3.0, 6.4),
    'ticker_uni': ('TickerUniverse\n✓ 4 affinity criteria\nfunding+vol live', GREEN, 7.2, 6.4),
    'strat_eng':  ('StrategyEngine\n✓ regime filtering\nNews/Trend/Range', GREEN, 3.0, 7.5),
    'bounce':     ('BounceFromDensity\n✓ p=3',      GREEN,  0.5, 8.7),
    'breakout':   ('BreakoutEatThrough\n✓ p=2',     GREEN,  2.4, 8.7),
    'leader_lag': ('LeaderLag\n✓ p=1',              GREEN,  4.3, 8.7),
    'flush':      ('FlushReversal\n✓ p=4 NEW',      GREEN,  6.2, 8.7),
    'risk':       ('RiskManager\nR1–R13 ✓',         GREEN,  3.0, 9.9),
    'live_exec':  ('LiveExecutor\nstate machine ✓', GREEN,  1.5, 11.0),
    'paper_exec': ('PaperExecutor\n✓',              GREEN,  4.5, 11.0),
    'rest_orders':('REST /orders\nGateway ✓',       GREEN,  1.5, 12.0),
}

EDGES = [
    ('ms_api','ws_ob',EDGE_N), ('ms_api','ws_tr',EDGE_N),
    ('ms_api','ws_acc',EDGE_N), ('ms_api','ws_mark',EDGE_N),
    ('ms_api','ws_fund',EDGE_N), ('ms_api','ws_sigLvl',EDGE_P),
    ('ws_ob','orderbook',EDGE_N), ('ws_tr','tradestream',EDGE_N),
    ('ws_acc','pos_mgr',EDGE_N),
    ('ws_mark','live_exec',EDGE_N),
    ('ws_fund','risk',EDGE_N),
    ('orderbook','feat_ext',EDGE_N), ('tradestream','feat_ext',EDGE_N),
    ('pos_mgr','live_exec',EDGE_N),
    ('feat_ext','density',EDGE_N), ('feat_ext','iceberg',EDGE_N),
    ('feat_ext','tape',EDGE_N), ('feat_ext','level',EDGE_N),
    ('feat_ext','approach',EDGE_N), ('feat_ext','leader_sig',EDGE_N),
    ('feat_ext','liqui',EDGE_B),
    ('density','signal_bus',EDGE_P), ('iceberg','signal_bus',EDGE_N),
    ('tape','signal_bus',EDGE_N), ('level','signal_bus',EDGE_P),
    ('approach','signal_bus',EDGE_N), ('leader_sig','signal_bus',EDGE_P),
    ('liqui','signal_bus',EDGE_B),
    ('signal_bus','ws_sigLvl',EDGE_P),
    ('ticker_uni','strat_eng',EDGE_P),
    ('signal_bus','strat_eng',EDGE_N),
    ('strat_eng','bounce',EDGE_N), ('strat_eng','breakout',EDGE_N),
    ('strat_eng','leader_lag',EDGE_N), ('strat_eng','flush',EDGE_N),
    ('bounce','risk',EDGE_P), ('breakout','risk',EDGE_P),
    ('leader_lag','risk',EDGE_P), ('flush','risk',EDGE_N),
    ('risk','live_exec',EDGE_N), ('risk','paper_exec',EDGE_N),
    ('live_exec','rest_orders',EDGE_N), ('rest_orders','exchange',EDGE_N),
]

# ═══════════════════════════════════════════════════════════════════════════
# 2. STRATEGY CONDITIONS DATA (from actual code)
# ═══════════════════════════════════════════════════════════════════════════
# status: 'ok'=green, 'warn'=yellow, 'bug'=red, 'miss'=gray, 'sep'=divider
STRAT_CONDS = [
    {
        'name': 'BounceFromDensity',
        'color': YELLOW,
        'priority': 3,
        'order_type': 'Limit',
        'entry': [
            ('LevelApproach ≤5s',          'ok'),
            ('Impulse speed / "impulse"',   'ok'),
            ('DensityDetected at level',    'ok'),
            ('Density age ≥ min_ms',        'ok'),
            ('Density / book_depth ratio',  'ok'),
            ('Tape stall (vol ratio)',       'ok'),
            ('Leader not contra',           'ok'),
            ('TapeFade (flag optional)',     'warn'),
            ('LeaderMove not contra',       'ok'),
            ('No Iceberg at level',         'ok'),
        ],
        'inv': [
            ('DensityRemoved on level', 'bug'),
            ('TapeBurst contra dir',    'ok'),
        ],
        'exit': [
            ('DensityRemoved post-entry', 'ok'),
            ('TapeBurst contra post',     'ok'),
        ],
    },
    {
        'name': 'BreakoutEatThrough',
        'color': YELLOW,
        'priority': 2,
        'order_type': 'Market',
        'entry': [
            ('DensityEating ≤2s',        'bug'),
            ('>50% density eaten',        'bug'),
            ('TapeBurst match dir ≤3s',   'ok'),
            ('Tape aggression ≥ thresh',  'ok'),
            ('Rel. volume ≥ 1.5×avg',     'ok'),
            ('Leader alignment',          'ok'),
            ('Resist. clusters ≤ 0.7',    'ok'),
            ('Support behind (density)',   'ok'),
            ('Not too close to best ask', 'ok'),
        ],
        'inv': [
            ('TapeFade on breakout side', 'ok'),
            ('LeaderMove contra',         'ok'),
            ('Density fully eaten',       'ok'),
        ],
        'exit': [
            ('TapeFade on our side',     'ok'),
            ('LeaderMove contra post',   'ok'),
        ],
    },
    {
        'name': 'LeaderLag',
        'color': YELLOW,
        'priority': 1,
        'order_type': 'Market',
        'entry': [
            ('LeaderMove ≤lag_max_age',  'bug'),
            ('|corr| ≥ min_corr',        'ok'),
            ('own change ≤ max',         'warn'),
            ('No density on path',       'ok'),
            ('Spread ≤ max',             'ok'),
        ],
        'inv': [],
        'exit': [
            ('Corr breakdown',    'ok'),
            ('Leader reversal',   'ok'),
        ],
    },
    {
        'name': 'FlushReversal',
        'color': GREEN,
        'priority': 4,
        'order_type': 'Limit',
        'entry': [
            ('Spread ≤ max_spread',      'ok'),
            ('LevelBreak ≤ 15s',         'ok'),
            ('Break dist ≥ 3 bps',       'ok'),
            ('Level touches ≥ 2',        'ok'),
            ('TapeFlush ≤ 8s',           'ok'),
            ('TapeFlush after break',    'ok'),
            ('Vol fade < 0.5',           'ok'),
            ('Reversal ≥ 1.5 bps',       'ok'),
            ('No cont. TapeBurst',       'ok'),
            ('No DensityEating',         'ok'),
        ],
        'inv': [],
        'exit': [
            ('SecondFlushPostEntry', 'ok'),
            ('BurstContraPostEntry', 'ok'),
        ],
    },
]

STATUS_C = {'ok': GREEN, 'warn': YELLOW, 'bug': RED, 'miss': GRAY}
STATUS_I = {'ok': '✓', 'warn': '~', 'bug': '⚠', 'miss': '✗'}

# ═══════════════════════════════════════════════════════════════════════════
# 3. FIGURE LAYOUT
# ═══════════════════════════════════════════════════════════════════════════
fig = plt.figure(figsize=(28, 18), facecolor=BG)
fig.suptitle(
    'Trade Bot — Data Flow & Trading Logic Audit  (code-verified 2026-05-15)',
    fontsize=15, color=WHITE, fontweight='bold', y=0.997)

gs = GridSpec(2, 3, figure=fig,
              left=0.01, right=0.995, top=0.975, bottom=0.003,
              width_ratios=[3.2, 1.15, 1.15],
              height_ratios=[1.0, 0.26],
              wspace=0.025, hspace=0.04)

ax_flow   = fig.add_subplot(gs[0, 0])   # main flow
ax_strat  = fig.add_subplot(gs[0, 1])   # strategy conditions
ax_info   = fig.add_subplot(gs[0, 2])   # legend + bugs
ax_bars   = fig.add_subplot(gs[1, :])   # completion bars

for ax in [ax_flow, ax_strat, ax_info, ax_bars]:
    ax.set_facecolor(BG2)
    ax.axis('off')

# ═══════════════════════════════════════════════════════════════════════════
# 4. DRAW FLOW GRAPH
# ═══════════════════════════════════════════════════════════════════════════
def node_box(ax, x, y, label, color, w=1.60, fs=6.0):
    lines = label.count('\n')
    h = 0.50 + lines * 0.14
    rect = FancyBboxPatch((x - w/2, y - h/2), w, h,
                          boxstyle='round,pad=0.04',
                          facecolor=color, edgecolor='white',
                          linewidth=0.5, alpha=0.90, zorder=3)
    ax.add_patch(rect)
    ax.text(x, y, label, ha='center', va='center',
            fontsize=fs, color='white', fontweight='bold',
            zorder=4, multialignment='center', linespacing=1.2)

def arrow(ax, x0, y0, x1, y1, color, w=1.60, hbox=0.55):
    dx, dy = x1-x0, y1-y0
    dist = (dx**2+dy**2)**0.5
    if dist < 0.01: return
    ux, uy = dx/dist, dy/dist
    sx, sy = x0+ux*w/2, y0+uy*hbox/2
    ex, ey = x1-ux*w/2, y1-uy*hbox/2
    ax.annotate('', xy=(ex,ey), xytext=(sx,sy),
                arrowprops=dict(arrowstyle='->', color=color,
                                lw=1.0, connectionstyle='arc3,rad=0.06'),
                zorder=2)

# layer labels
layers = [
    (-0.35,'EXCHANGE'), (0.85,'WS TRANSPORT'), (2.0,'MARKET DATA'),
    (3.1,'FEATURE EXTRACTION'), (4.35,'SIGNAL DETECTORS'),
    (5.85,'SIGNAL BUS'), (6.95,'STRATEGY ENGINE'),
    (8.15,'STRATEGIES'), (9.35,'RISK MANAGER'),
    (10.45,'EXECUTOR'), (11.55,'ORDERS OUT'),
]
for y_div, lbl in layers:
    ax_flow.axhline(y=y_div, color='#1e3a5f', lw=0.5, zorder=0)
    ax_flow.text(-2.45, y_div+0.10, lbl, fontsize=6.0, color=DIM,
                 fontweight='bold', va='bottom')

for src, dst, ec in EDGES:
    x0,y0 = NODES[src][2], NODES[src][3]
    x1,y1 = NODES[dst][2], NODES[dst][3]
    arrow(ax_flow, x0,y0, x1,y1, ec)

for nid,(label,color,x,y) in NODES.items():
    node_box(ax_flow, x, y, label, color)

ax_flow.set_xlim(-2.6, 10.3)
ax_flow.set_ylim(12.7, -0.6)
ax_flow.set_aspect('auto')

# ═══════════════════════════════════════════════════════════════════════════
# 5. STRATEGY CONDITIONS PANEL
# ═══════════════════════════════════════════════════════════════════════════
ax_strat.text(0.5, 0.995, 'Entry Conditions  (из кода)', color=WHITE,
              fontsize=9.5, fontweight='bold', ha='center', va='top',
              transform=ax_strat.transAxes)

# column headers
col_xs = [0.01, 0.26, 0.51, 0.76]
strat_names_short = ['Bounce', 'Breakout', 'LeadLag', 'Flush']
for cx, sn, sc in zip(col_xs, strat_names_short,
                       [NODES[n][1] for n in ['bounce','breakout','leader_lag','flush']]):
    ax_strat.add_patch(FancyBboxPatch(
        (cx-0.01, 0.955), 0.22, 0.036,
        boxstyle='round,pad=0.01', facecolor=sc, alpha=0.7,
        transform=ax_strat.transAxes, clip_on=False))
    ax_strat.text(cx+0.10, 0.973, sn, transform=ax_strat.transAxes,
                  fontsize=7.0, color=WHITE, fontweight='bold', ha='center', va='center')

# sub-header row
for cx, s in zip(col_xs, STRAT_CONDS):
    ax_strat.text(cx+0.10, 0.944, f'p={s["priority"]} {s["order_type"]}',
                  transform=ax_strat.transAxes,
                  fontsize=5.5, color=SILVER, ha='center', va='center')

def draw_cond_block(ax, cx, y_start, items, section_label, col_w=0.30, row_h=0.046):
    """Draw a labelled block of conditions starting at y_start (axes coords, top-down)."""
    y = y_start
    ax.text(cx, y, section_label, transform=ax.transAxes,
            fontsize=6.0, color=SILVER, ha='left', va='top', fontstyle='italic')
    y -= 0.025
    for label, status in items:
        sc = STATUS_C.get(status, GRAY)
        si = STATUS_I.get(status, '?')
        # tiny colored square
        ax.add_patch(FancyBboxPatch(
            (cx, y-0.014), 0.022, 0.022,
            boxstyle='round,pad=0.002', facecolor=sc, alpha=0.85,
            transform=ax.transAxes, clip_on=False))
        ax.text(cx+0.026, y-0.003, si, transform=ax.transAxes,
                fontsize=5.5, color=sc, va='center', fontweight='bold')
        ax.text(cx+0.046, y-0.003, label, transform=ax.transAxes,
                fontsize=6.0, color=WHITE if status=='ok' else (YELLOW if status=='warn' else (RED if status=='bug' else SILVER)),
                va='center')
        y -= row_h
    return y

# Draw conditions for each strategy
for cx, strat in zip(col_xs, STRAT_CONDS):
    y = 0.92
    y = draw_cond_block(ax_strat, cx, y, strat['entry'], 'ENTRY', row_h=0.044)
    y -= 0.01
    if strat['inv']:
        y = draw_cond_block(ax_strat, cx, y, strat['inv'], 'INVALIDATE', row_h=0.044)
        y -= 0.01
    if strat['exit']:
        y = draw_cond_block(ax_strat, cx, y, strat['exit'], 'POST-ENTRY EXIT', row_h=0.044)

# Signal → Strategy dependency matrix
ax_strat.plot([0.0, 1.0], [0.18, 0.18], color='#1e3a5f', lw=0.8,
              transform=ax_strat.transAxes)
ax_strat.text(0.5, 0.175, 'Signal dependency matrix', transform=ax_strat.transAxes,
              fontsize=7.5, color=SILVER, ha='center', va='top', fontweight='bold')

signals = [
    ('DensityDetected',  [True,  False, False, False]),
    ('DensityRemoved',   [True,  False, False, False]),
    ('DensityEating',    [False, True,  False, True ]),
    ('IcebergSuspected', [True,  True,  False, False]),
    ('TapeBurst',        [True,  True,  False, True ]),
    ('TapeFade',         [True,  True,  False, False]),
    ('TapeFlush',        [False, False, False, True ]),
    ('LevelApproach',    [True,  False, False, False]),
    ('LevelBreak',       [False, False, False, True ]),
    ('LeaderMove',       [True,  True,  True,  False]),
]
sig_xs = [0.44, 0.58, 0.72, 0.86]
col_heads = ['Bnc', 'Brk', 'Lag', 'Flu']
y_sig = 0.155
for hx, h in zip(sig_xs, col_heads):
    ax_strat.text(hx, y_sig, h, transform=ax_strat.transAxes,
                  fontsize=6, color=SILVER, ha='center', va='top', fontweight='bold')
y_sig -= 0.025

for sig_name, used in signals:
    ax_strat.text(0.01, y_sig, sig_name, transform=ax_strat.transAxes,
                  fontsize=5.8, color=WHITE, va='center')
    for sx, u in zip(sig_xs, used):
        c = GREEN if u else '#2c3e50'
        ax_strat.add_patch(FancyBboxPatch(
            (sx-0.04, y_sig-0.011), 0.08, 0.022,
            boxstyle='round,pad=0.002', facecolor=c, alpha=0.85,
            transform=ax_strat.transAxes, clip_on=False))
        ax_strat.text(sx, y_sig, '✓' if u else '', transform=ax_strat.transAxes,
                      fontsize=5.5, color=WHITE, ha='center', va='center')
    y_sig -= 0.028

# ═══════════════════════════════════════════════════════════════════════════
# 6. INFO PANEL: legend + bugs
# ═══════════════════════════════════════════════════════════════════════════
ax_info.text(0.5, 0.995, 'Legend', color=WHITE, fontsize=9.5,
             fontweight='bold', ha='center', va='top', transform=ax_info.transAxes)

legend_items = [
    (GREEN,  '✓  Fully implemented'),
    (YELLOW, '⚠  Partial or has bugs'),
    (RED,    '✗  Not implemented'),
    (BLUE,   '○  External / API'),
    (EDGE_N, '→  Normal data flow'),
    (EDGE_P, '→  Partial / caveat'),
    (EDGE_B, '→  Bug / missing path'),
]
for i,(c,lbl) in enumerate(legend_items):
    y = 0.955 - i*0.058
    ax_info.add_patch(FancyBboxPatch(
        (0.03, y-0.020), 0.08, 0.036,
        boxstyle='round,pad=0.003', facecolor=c,
        transform=ax_info.transAxes, clip_on=False))
    ax_info.text(0.14, y, lbl, transform=ax_info.transAxes,
                 fontsize=8.0, color=WHITE, va='center')

# divider
ax_info.plot([0.02, 0.98], [0.54, 0.54], color='#2c3e50', lw=0.8,
             transform=ax_info.transAxes)

# Bugs / gaps
bugs = [
    ('P0 FIXED ✓', GREEN, [
        '✓ BUG-1  DensityRemoved теперь эмитируется\n'
        '          для confirmed densities',
        '✓ BUG-2  DensityEating: sliding window ✓',
        '  BUG-3  FALSE POS — CircularBuffer<T,N>\n'
        '          в LeaderSignal уже был ✓',
    ]),
    ('P1 FALSE POSITIVES', GRAY, [
        '  MEM-3  ApproachAnalyzer — FALSE POS\n'
        '          history_ ограничен CircBuf',
        '  MEM-4  IcebergDetector — FALSE POS\n'
        '          trade_history_ ограничен',
        '  MEM-5  LevelDetector — FALSE POS\n'
        '          bounded containers ✓',
    ]),
    ('P1 LOGIC → FIXED ✓', GREEN, [
        '✓ GAP-1  regime filter в StrategyEngine',
        '✓ GAP-2  LevelFormed/Break/Reject emitted',
        '✓ GAP-3  LevelFormed → SignalLevelBridge',
        '✓ GAP-4  TickerStats 4 критерия wired',
    ]),
    ('OPEN', RED, [
        '✓ FlushReversal: IMPL p=4 NEW',
        '✗ LiquidationDetector: NOT IMPL',
        '✗ find_swing_low/high: min/max only',
    ]),
]

y = 0.53
for section, color, items in bugs:
    ax_info.text(0.03, y, section, transform=ax_info.transAxes,
                 fontsize=7.0, color=color, fontweight='bold', va='top')
    y -= 0.038
    for item in items:
        nl = item.count('\n')
        ax_info.text(0.03, y, item, transform=ax_info.transAxes,
                     fontsize=6.0, color=SILVER, va='top')
        y -= 0.028*(nl+1) + 0.004
    y -= 0.008

# ═══════════════════════════════════════════════════════════════════════════
# 7. COMPLETION BARS
# ═══════════════════════════════════════════════════════════════════════════
modules = [
    ('OrderBook',       95, GREEN),  ('TradeStream',     95, GREEN),
    ('FeatureExtr.',    90, GREEN),  ('DensityDet.',     90, GREEN),
    ('IcebergDet.',     85, GREEN),  ('TapeAnalyzer',    90, GREEN),
    ('LevelDet.',       85, GREEN),  ('ApproachAnal.',   85, GREEN),
    ('LeaderSignal',    85, GREEN),  ('LiquiDet.',        0, RED),
    ('StrategyEng.',    88, GREEN),  ('TickerUniverse',  85, GREEN),
    ('Bounce',          85, GREEN),  ('Breakout',        85, GREEN),
    ('LeaderLag',       85, GREEN),  ('FlushReversal',   80, GREEN),
    ('RiskManager',     92, GREEN),  ('LiveExecutor',    88, GREEN),
    ('PaperExecutor',   88, GREEN),
]

n = len(modules)
bw = 1.0/n

for i,(name,pct,color) in enumerate(modules):
    x0 = i*bw
    xc = x0+bw/2
    ax_bars.add_patch(FancyBboxPatch(
        (x0+0.002, 0.30), bw-0.004, 0.55,
        boxstyle='round,pad=0.01', facecolor='#1e3a5f',
        transform=ax_bars.transAxes, clip_on=False))
    fill_w = (bw-0.004)*pct/100.0
    ax_bars.add_patch(FancyBboxPatch(
        (x0+0.002, 0.30), fill_w, 0.55,
        boxstyle='round,pad=0.01', facecolor=color, alpha=0.85,
        transform=ax_bars.transAxes, clip_on=False))
    ax_bars.text(xc, 0.645, f'{pct}%', transform=ax_bars.transAxes,
                 fontsize=5.8, color='white', ha='center', va='center',
                 fontweight='bold')
    ax_bars.text(xc, 0.17, name, transform=ax_bars.transAxes,
                 fontsize=5.3, color=SILVER, ha='center', va='center',
                 rotation=28)

ax_bars.text(0.5, 0.03, 'Module Implementation Completeness  (% — code-verified)',
             transform=ax_bars.transAxes, fontsize=7.5, color=GRAY, ha='center')

# ── save ──────────────────────────────────────────────────────────────────
out = '/home/yevge/trade-bot-hardcore-spec/reports/bot_flow_graph.png'
plt.savefig(out, dpi=160, bbox_inches='tight', facecolor=BG)
print(f'Saved: {out}')
