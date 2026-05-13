'use strict';

// @depends-on: dom.js

/**
 * Toast notification system.
 */
function toast(title, msg, type) {
  const t = el('div', 'toast');
  t.className = 'toast ' + (type || '');
  const progress = el('div', 'toast-progress');
  t.appendChild(progress);
  
  const strong = el('strong');
  strong.textContent = title;
  
  const span = el('span');
  span.style.cssText = 'font-size:11px;color:var(--muted)';
  span.innerHTML = msg;
  
  t.appendChild(strong);
  t.appendChild(el('br'));
  t.appendChild(span);
  
  const container = $('toasts');
  if (container) {
    container.appendChild(t);
    setTimeout(() => {
      t.style.opacity = '0';
      t.style.transition = 'opacity 0.3s';
      setTimeout(() => t.remove(), 300);
    }, 4800);
  }
}
