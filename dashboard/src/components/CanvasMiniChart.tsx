import React, { useRef, useEffect } from 'react';

export function CanvasMiniChart({ data, color }: { data: number[], color: string }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const w = canvas.width;
    const h = canvas.height;

    ctx.clearRect(0, 0, w, h);
    if (!data || data.length === 0) return;

    const max = Math.max(...data, 1);
    const min = Math.min(...data, 0);
    const range = max - min === 0 ? 1 : max - min;
    
    // Draw
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    
    const step = w / (data.length - 1);
    
    data.forEach((val, i) => {
      const x = i * step;
      const y = h - ((val - min) / range) * h;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    
    ctx.stroke();
    
    // Fill
    ctx.lineTo(w, h);
    ctx.lineTo(0, h);
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
