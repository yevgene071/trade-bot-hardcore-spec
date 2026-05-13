'use strict';

/**
 * Global dashboard state.
 * Phase 3: Consolidate scattered state into a single module.
 */
let _state = null;
let _prevTrades = new Map();
let _startTime = Date.now();
let _selTicker = 'BTC_USDT';
let _selTickerTime = 0;
let _hmFilter = 'ALL';
let _tfPoints = 300;
let _sigFilter = '';
let _uniFilter = '';
let _seenSignals = new Set();
let _expandedCell = null;
let _expandedPos = null;
let _groupOpen = {};

let _lastChartLen = 0;
let _lastChartLastMid = 0;
let _lastTradingHash = '';
let _lastSticks = 0;
let _resizeTimer = null;

// Chart zoom state
let _chartZoom = null; // { start: idx, end: idx } or null
let _zoomDragStart = null; // { x: px, idx: number } or null

// Incremental signal feed tracking
let _renderedSigKeys = new Set();
let _renderedSigFullKeys = new Set();
let _lastSigFilter = '';

/**
 * State setters to be used by other modules.
 */
function setState(s) { _state = s; }
function setSelTicker(t) { _selTicker = t; }
function setSelTickerTime(t) { _selTickerTime = t; }
function setHmFilter(f) { _hmFilter = f; }
function setTfPoints(p) { _tfPoints = p; }
function setSigFilter(f) { _sigFilter = f; }
function setUniFilter(f) { _uniFilter = f; }
function setChartZoom(z) { _chartZoom = z; }
function setZoomDragStart(s) { _zoomDragStart = s; }
function setLastTradingHash(h) { _lastTradingHash = h; }
function setLastSticks(s) { _lastSticks = s; }
function setResizeTimer(t) { _resizeTimer = t; }
