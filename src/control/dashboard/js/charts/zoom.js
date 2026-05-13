'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/state.js

/**
 * Chart zoom logic.
 * Phase 5: Extract updateZoomIndicator, initChartZoom, xToIdx, drag/wheel handlers.
 */

function updateZoomIndicator() {
  const el = $('zoom-indicator');
  if (!el) return;
  if (_chartZoom !== null) {
    el.style.display = 'inline';
    $('zoom-range').textContent = `${_chartZoom.start}–${_chartZoom.end}`;
  } else {
    el.style.display = 'none';
  }
}

function initChartZoom() {
  const container = $('trade-chart-body');
  const overlay = $('zoom-overlay');
  const select = $('zoom-select');
  if (!container || !overlay || !select) return;

  function xToIdx(px, dataLen) {
    const w = container.clientWidth;
    const padL = 10, padR = 45;
    const pw = w - padL - padR;
    if (pw <= 0 || dataLen <= 1) return 0;
    return Math.max(0, Math.min(dataLen - 1, Math.round(((px - padL) / pw) * (dataLen - 1))));
  }

  // Drag-to-zoom
  container.addEventListener('mousedown', (e) => {
    if (e.button !== 0 || e.target.closest('#zoom-indicator')) return;
    const rect = container.getBoundingClientRect();
    const px = e.clientX - rect.left;
    const totalLen = (_state?.chart_history || []).length;
    if (totalLen < 5) return;
    setZoomDragStart({ x: px, idx: xToIdx(px, totalLen) });
    overlay.style.display = 'block';
    select.style.display = 'none';
  });

  container.addEventListener('mousemove', (e) => {
    if (!_zoomDragStart) return;
    const rect = container.getBoundingClientRect();
    const px = Math.max(0, Math.min(container.clientWidth, e.clientX - rect.left));
    const x1 = Math.min(_zoomDragStart.x, px);
    const x2 = Math.max(_zoomDragStart.x, px);
    select.style.display = 'block';
    select.style.left = x1 + 'px';
    select.style.width = (x2 - x1) + 'px';
  });

  container.addEventListener('mouseup', (e) => {
    if (!_zoomDragStart) return;
    const rect = container.getBoundingClientRect();
    const px = Math.max(0, Math.min(container.clientWidth, e.clientX - rect.left));
    const totalLen = (_state?.chart_history || []).length;
    const startIdx = Math.min(_zoomDragStart.idx, xToIdx(px, totalLen));
    const endIdx = Math.max(_zoomDragStart.idx, xToIdx(px, totalLen));
    overlay.style.display = 'none';
    select.style.display = 'none';
    setZoomDragStart(null);
    if (endIdx - startIdx >= 3) {
      setChartZoom({ start: startIdx, end: endIdx });
      updateZoomIndicator();
      if (_state) renderTrading(_state, true);
    }
  });

  // Double-click to reset zoom
  container.addEventListener('dblclick', () => {
    setChartZoom(null);
    updateZoomIndicator();
    if (_state) renderTrading(_state, true);
  });

  // Mouse wheel zoom in/out
  container.addEventListener('wheel', (e) => {
    if (!_state?.chart_history?.length) return;
    e.preventDefault();
    const totalLen = _state.chart_history.length;
    const rect = container.getBoundingClientRect();
    const cursorRatio = Math.max(0, Math.min(1, (e.clientX - rect.left) / container.clientWidth));
    
    if (_chartZoom === null) {
      // Start zoom around cursor
      const range = Math.round(totalLen * 0.4);
      const center = Math.round(cursorRatio * (totalLen - 1));
      setChartZoom({
        start: Math.max(0, center - Math.round(range * cursorRatio)),
        end: Math.min(totalLen - 1, center + Math.round(range * (1 - cursorRatio)))
      });
    } else {
      // Zoom in/out around cursor
      const delta = e.deltaY > 0 ? 1.2 : 1 / 1.2;
      const range = _chartZoom.end - _chartZoom.start;
      const center = _chartZoom.start + range * cursorRatio;
      const newRange = Math.min(Math.max(3, Math.round(range * delta)), totalLen - 1);
      const newStart = Math.max(0, Math.round(center - newRange * cursorRatio));
      setChartZoom({
        start: newStart,
        end: Math.min(totalLen - 1, newStart + newRange)
      });
    }
    updateZoomIndicator();
    renderTrading(_state, true);
  }, { passive: false });

  // Click on zoom indicator to reset
  const indicator = $('zoom-indicator');
  if (indicator) {
    indicator.addEventListener('click', () => {
      setChartZoom(null);
      updateZoomIndicator();
      if (_state) renderTrading(_state, true);
    });
  }

  // Custom timeframe input
  const tfCustom = $('tf-custom');
  if (tfCustom && !tfCustom.dataset.tfInit) {
    tfCustom.dataset.tfInit = '1';
    tfCustom.addEventListener('change', () => {
      const v = parseInt(tfCustom.value);
      if (v >= 10 && v <= 300) {
        setTfPoints(v);
        document.querySelectorAll('#trade-tf-buttons .tf-btn').forEach(b => b.classList.remove('active'));
        setChartZoom(null);
        updateZoomIndicator();
        if (_state) renderTrading(_state);
      }
    });
  }
}
