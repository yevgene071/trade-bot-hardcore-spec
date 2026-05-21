import React, { useRef, useEffect } from 'react';

export function CanvasMiniChart({ data, color }: { data: number[], color: string }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    const displayW = canvas.clientWidth || 150;
    const displayH = canvas.clientHeight || 40;
    const w = displayW * dpr;
    const h = displayH * dpr;

    // Only resize when needed to avoid clearing every render
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    ctx.clearRect(0, 0, displayW, displayH);
    if (!data || data.length === 0) return;

    const max = Math.max(...data, 1);
    const min = Math.min(...data, 0);
    const range = max - min === 0 ? 1 : max - min;
    
    // Draw
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    
    // Guard: single-point datasets have no line to draw.
    if (data.length < 2) {
      const val = data[0];
      const y = displayH - ((val - min) / range) * displayH;
      ctx.arc(displayW / 2, y, 2, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();
      ctx.stroke();
      return;
    }

    const step = displayW / (data.length - 1);
    
    data.forEach((val, i) => {
      const x = i * step;
      const y = displayH - ((val - min) / range) * displayH;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    
    ctx.stroke();
    
    // Fill
    ctx.lineTo(displayW, displayH);
    ctx.lineTo(0, displayH);
    ctx.closePath();
    ctx.fillStyle = color + '20'; // 12% opacity roughly hex + '20'
    ctx.fill();
    
  }, [data, color]);

  return (
    <canvas 
      ref={canvasRef} 
      width={150} 
      height={40} 
      className="w-full h-full" 
    />
  );
}
