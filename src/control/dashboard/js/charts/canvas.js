'use strict';

/**
 * Canvas base utilities.
 * Phase 5: Extract drawCanvas + utils.
 */

function setupCanvas(canvas) {
  if (!canvas || !canvas.getContext) return { ctx: null, w: 0, h: 0 };
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth;
  const cssH = canvas.clientHeight;
  const needResize = canvas.width !== Math.round(cssW * dpr) || canvas.height !== Math.round(cssH * dpr);
  if (needResize) {
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
    canvas.style.width = cssW + 'px';
    canvas.style.height = cssH + 'px';
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);
  return { ctx, w: cssW, h: cssH };
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
