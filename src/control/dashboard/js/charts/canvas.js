'use strict';

/**
 * Canvas base utilities.
 * Phase 5: Extract drawCanvas + utils.
 */

function setupCanvas(canvas) {
  const ctx = canvas.getContext('2d');
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}

function drawGrid(ctx, w, h, pad, rows = 4) {
  ctx.strokeStyle = 'rgba(255,255,255,0.04)';
  ctx.lineWidth = 1;
  const ph = h - pad.top - pad.bottom;
  for (let i = 0; i <= rows; i++) {
    const y = pad.top + (i / rows) * ph;
    ctx.beginPath();
    ctx.moveTo(pad.left, y);
    ctx.lineTo(w - pad.right, y);
    ctx.stroke();
  }
}
