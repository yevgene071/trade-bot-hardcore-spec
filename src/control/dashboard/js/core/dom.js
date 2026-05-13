'use strict';

// @depends-on: state.js

/**
 * DOM manipulation helpers.
 */
const $ = id => document.getElementById(id);

function el(tag, cls, txt) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (txt !== undefined) e.textContent = txt;
  return e;
}

function sigClass(kind) {
  const m = {
    'DensityDetected': 'sig-DensityDetected',
    'DensityRemoved': 'sig-DensityRemoved',
    'DensityEating': 'sig-DensityEating',
    'IcebergSuspected': 'sig-IcebergSuspected',
    'TapeBurst': 'sig-TapeBurst',
    'TapeFade': 'sig-TapeFade',
    'TapeFlush': 'sig-TapeFlush',
    'TapeDistribution': 'sig-TapeDistribution',
    'LevelFormed': 'sig-LevelFormed',
    'LevelApproach': 'sig-LevelApproach',
    'LevelRejection': 'sig-LevelRejection',
    'LevelBreak': 'sig-LevelBreak',
    'LeaderMove': 'sig-LeaderMove'
  };
  return m[kind] || '';
}

function stratBadge(name) {
  if (!name) return '';
  const cls = name.includes('Bounce') ? 'strat-bounce' : name.includes('Breakout') ? 'strat-breakout' : name.includes('Leader') ? 'strat-leaderlag' : '';
  const short = name.replace('FromDensity', '').replace('EatThrough', '');
  return `<span class="strat-badge ${cls}">${short}</span>`;
}

function stClass(rs) {
  const m = ['st-cold', 'st-warming', 'st-ready', 'st-planning', 'st-trading', 'st-cooldown'];
  return m[rs] || 'st-cold';
}

function stName(rs) {
  const m = ['Cold', 'Warming', 'Ready', 'Planning', 'Trading', 'Cooldown'];
  return m[rs] || 'Cold';
}

function stSymbol(rs) {
  const m = ['⬤', '●', '◉', '◆', '■', '○'];
  return m[rs] || '⬤';
}
