'use strict';

// @depends-on: state.js

/**
 * Formatting utilities for the dashboard.
 */
const fmt$ = (v) =>
  '$' +
  Number(v || 0).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const fmtS = (v, d = 2) => (v > 0 ? '+' : '') + Number(v || 0).toFixed(d);
const fmtPct = (v, d = 2) => (v >= 0 ? '+' : '') + Number(v || 0).toFixed(d) + '%';
const fmtT = (v) => Number(v || 0).toFixed(5);
const fmtT2 = (v) => Number(v || 0).toFixed(2);
const fmtT4 = (v) => Number(v || 0).toFixed(4);
const fmtSz = (v) => {
  const n = Number(v || 0);
  return n >= 1000 ? (n / 1000).toFixed(1) + 'K' : n >= 1 ? n.toFixed(1) : n.toFixed(4);
};
