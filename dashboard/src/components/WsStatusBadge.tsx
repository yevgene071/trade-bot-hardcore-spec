import { useState, useEffect, useRef } from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { cn } from '../lib/utils';

const RECONNECT_MS = 2000;

export function WsStatusBadge() {
  const { wsStatus, latency, reconnectAttempt, lastReconnectAt, wsMsgCount } = useTradeStore();
  const [open, setOpen] = useState(false);
  const [remainingMs, setRemainingMs] = useState(0);
  const [msgRate, setMsgRate] = useState(0);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const closeTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const prevCountRef = useRef(wsMsgCount);

  // Cleanup close timer on unmount
  useEffect(() => {
    return () => {
      if (closeTimerRef.current) clearTimeout(closeTimerRef.current);
    };
  }, []);

  // Live countdown when reconnecting
  useEffect(() => {
    if (wsStatus === 'reconnecting' && lastReconnectAt != null) {
      const tick = () => {
        const elapsed = Date.now() - lastReconnectAt;
        setRemainingMs(Math.max(0, RECONNECT_MS - elapsed));
      };
      tick();
      timerRef.current = setInterval(tick, 100);
    } else {
      if (timerRef.current) clearInterval(timerRef.current);
      timerRef.current = null;
      setRemainingMs(0);
    }
    return () => {
      if (timerRef.current) clearInterval(timerRef.current);
    };
  }, [wsStatus, lastReconnectAt]);

  // Compute msg/s rate every second from wsMsgCount delta.
  // NOTE: depends on [wsStatus] ONLY. Depending on wsMsgCount re-created the
  // interval on every incoming message, so under real load (continuous
  // stream) the 1s timer almost never fired and the rate stuck at 0 — i.e.
  // the metric broke exactly when it mattered. Read the live count via
  // getState() inside the tick instead.
  useEffect(() => {
    if (wsStatus !== 'connected') {
      setMsgRate(0);
      return;
    }
    prevCountRef.current = useTradeStore.getState().wsMsgCount;
    const interval = setInterval(() => {
      const currentCount = useTradeStore.getState().wsMsgCount;
      const delta = Math.max(0, currentCount - prevCountRef.current);
      prevCountRef.current = currentCount;
      setMsgRate(delta);
    }, 1000);
    return () => clearInterval(interval);
  }, [wsStatus]);

  // Hover open/close with delay to prevent flicker
  const handleMouseEnter = () => {
    if (closeTimerRef.current) clearTimeout(closeTimerRef.current);
    setOpen(true);
  };
  const handleMouseLeave = () => {
    closeTimerRef.current = setTimeout(() => setOpen(false), 150);
  };

  const statusColor = wsStatus === 'connected' ? 'bg-[#1a9448]'
    : wsStatus === 'connecting' || wsStatus === 'reconnecting' ? 'bg-[#c08828]'
    : 'bg-[#ae2c2c]';

  const statusTextColor = wsStatus === 'connected' ? 'text-[#1a9448]'
    : wsStatus === 'connecting' || wsStatus === 'reconnecting' ? 'text-[#c08828]'
    : 'text-[#ae2c2c]';

  let label: string;
  if (wsStatus === 'connected') label = `${latency}ms`;
  else if (wsStatus === 'connecting') label = 'connecting…';
  else if (wsStatus === 'reconnecting') label = `rty ${reconnectAttempt}…`;
  else label = 'offline';

  return (
    <div className="relative" onMouseEnter={handleMouseEnter} onMouseLeave={handleMouseLeave}>
      {/* Trigger */}
      <div className="flex items-center gap-1.5 cursor-pointer">
        <div className={cn('w-[5px] h-[5px] rounded-full', statusColor, wsStatus !== 'disconnected' && 'animate-pulse')} />
        <span className={cn('font-mono text-[8px] tracking-[.06em]', statusTextColor)}>
          {label}
        </span>
      </div>

      {/* Tooltip */}
      {open && (
        <div
          className="absolute top-full right-0 mt-1.5 z-[100] min-w-[184px]"
          onMouseEnter={handleMouseEnter}
          onMouseLeave={handleMouseLeave}
        >
          <div className="bg-[rgba(10,15,26,0.97)] backdrop-blur-xl border border-[#1c2a38] rounded-lg shadow-[0_8px_32px_rgba(0,0,0,0.7)] px-3 py-2.5 space-y-1.5">
            {/* Header */}
            <div className="flex items-center gap-2 pb-1.5 border-b border-[#141e30]">
              <div className={cn('w-2 h-2 rounded-full', statusColor, wsStatus !== 'disconnected' && 'animate-pulse')} />
              <span className={cn('text-[10px] font-bold uppercase tracking-[.12em]', statusTextColor)}>
                {wsStatus === 'connected' && 'Connected'}
                {wsStatus === 'connecting' && 'Connecting…'}
                {wsStatus === 'reconnecting' && 'Reconnecting…'}
                {wsStatus === 'disconnected' && 'Disconnected'}
              </span>
            </div>

            {/* Latency */}
            {wsStatus === 'connected' && (
              <>
                <div className="flex justify-between items-center">
                  <span className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Latency</span>
                  <span className="text-[9px] font-mono font-semibold text-[#1a9448]">{latency} ms</span>
                </div>
                <div className="flex justify-between items-center">
                  <span className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Messages</span>
                  <span className="text-[9px] font-mono font-semibold text-[#b0c0d4]">{wsMsgCount.toLocaleString()}</span>
                </div>
                <div className="flex justify-between items-center">
                  <span className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Rate</span>
                  <span className="text-[9px] font-mono font-semibold text-[#71a6e6]">{msgRate}/s</span>
                </div>
              </>
            )}

            {/* Reconnect attempts */}
            {(wsStatus === 'reconnecting' || wsStatus === 'connecting') && (
              <>
                <div className="flex justify-between items-center">
                  <span className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Attempts</span>
                  <span className="text-[9px] font-mono font-semibold text-[#c08828]">{reconnectAttempt}</span>
                </div>
                <div className="flex justify-between items-center">
                  <span className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Next retry</span>
                  <span className="text-[9px] font-mono font-semibold text-[#b0c0d4]">
                    {(remainingMs / 1000).toFixed(1)}s
                  </span>
                </div>
              </>
            )}

            {/* Last reconnect time */}
            {wsStatus === 'connected' && reconnectAttempt > 0 && lastReconnectAt != null && (
              <div className="flex justify-between items-center">
                <span className="text-[8px] text-[#3c4e62] uppercase tracking-[.08em]">Reconnects</span>
                <span className="text-[9px] font-mono font-semibold text-[#1a9448]">{reconnectAttempt}</span>
              </div>
            )}

            {/* Progress bar when reconnecting */}
            {wsStatus === 'reconnecting' && (
              <div className="w-full h-1 bg-[#0c1220] rounded-full overflow-hidden mt-0.5">
                <div
                  className="h-full bg-[#c08828] rounded-full transition-none"
                  style={{ width: `${(1 - remainingMs / RECONNECT_MS) * 100}%` }}
                />
              </div>
            )}

            {/* Offline note */}
            {wsStatus === 'disconnected' && (
              <div className="text-[8px] text-[#3c4e62] italic">Backend unreachable</div>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
