'use strict';

/**
 * Mini chart renderer.
 * Phase 5: Extract drawMiniChart.
 */
function drawMiniChart(ctx, w, h, history, field, color, label) {
  if (history.length < 2) { 
    ctx.fillStyle = '#7b859c'; 
    ctx.font = '9px sans-serif'; 
    ctx.textAlign = 'center'; 
    ctx.fillText(label, w / 2, h / 2); 
    return; 
  }
  const pad = { top: 4, right: 8, bottom: 14, left: 8 }, pw = w - pad.left - pad.right, ph = h - pad.top - pad.bottom;
  const vals = history.map(p => p[field] || 0);
  const min = Math.min(...vals), max = Math.max(...vals), range = (max - min) || 1;
  const toX = i => pad.left + (i / (vals.length - 1)) * pw, toY = v => pad.top + ph - ((v - min) / range) * ph;
  
  ctx.beginPath(); 
  ctx.moveTo(toX(0), toY(vals[0])); 
  vals.forEach((v, i) => ctx.lineTo(toX(i), toY(v)));
  ctx.strokeStyle = color; 
  ctx.lineWidth = 1; 
  ctx.stroke();

  // Zero line
  if (min < 0 && max > 0) { 
    const zy = toY(0); 
    ctx.strokeStyle = 'rgba(255,255,255,0.1)'; 
    ctx.beginPath(); 
    ctx.moveTo(pad.left, zy); 
    ctx.lineTo(w - pad.right, zy); 
    ctx.stroke(); 
  }
  
  ctx.fillStyle = '#7b859c'; 
  ctx.font = '7px Inter, sans-serif'; 
  ctx.textAlign = 'left';
  ctx.fillText(max.toFixed(1), 2, pad.top + 10);
  ctx.fillText(min.toFixed(1), 2, h - 4);
}
