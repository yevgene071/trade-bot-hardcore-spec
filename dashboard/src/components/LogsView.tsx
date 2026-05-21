import React, { useState, useEffect, useRef } from 'react';
import { apiGetLogs } from '../transport/api';
import { cn } from '../lib/utils';

export function LogsView() {
  const [logs, setLogs] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const bottomRef = useRef<HTMLDivElement>(null);

  const fetchLogs = async (silent = false) => {
    if (!silent) setLoading(true);
    try {
      const data = await apiGetLogs(200);
      setLogs(Array.isArray(data) ? data : []);
      setError(null);
    } catch (e: any) {
      setError(e.message);
    } finally {
      if (!silent) setLoading(false);
    }
  };

  // Realtime: poll every 3s in the background (silent, no spinner flicker)
  // in addition to the manual Refresh button.
  useEffect(() => {
    fetchLogs();
    const id = setInterval(() => fetchLogs(true), 3000);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [logs]);

  return (
    <div className="h-full flex flex-col bg-[#070b13]">
      <div className="h-7 bg-[#0a0f1a] border-b border-[#141e30] flex items-center justify-between px-2.5 shrink-0">
        <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.12em]">Bot Logs</span>
        <button
          onClick={() => fetchLogs()}
          disabled={loading}
          className="px-2 py-0.5 text-[8px] font-bold uppercase tracking-wider rounded border border-[#141e30] text-[#3c4e62] hover:text-[#b0c0d4] hover:border-[#3c4e62] transition-colors disabled:opacity-40"
        >
          {loading ? 'Loading...' : 'Refresh'}
        </button>
      </div>
      <div className="flex-1 overflow-y-auto no-scrollbar font-mono text-[11px] p-3 space-y-0.5">
        {error && (
          <div className="text-[#ae2c2c] p-2 border border-[#ae2c2c]/20 rounded bg-[#ae2c2c]/5">
            Error: {error}
          </div>
        )}
        {logs.length === 0 && !loading && !error && (
          <div className="text-[#3c4e62] text-center mt-10">No logs</div>
        )}
        {logs.map((line, i) => (
          <div key={`${i}:${line.slice(0, 48)}`} className={cn(
            "leading-relaxed whitespace-pre-wrap break-all",
            line.includes('ERROR') ? "text-[#ae2c2c]" :
            line.includes('WARN') ? "text-[#c08828]" :
            "text-[#3c4e62] hover:text-[#b0c0d4]"
          )}>
            {line}
          </div>
        ))}
        <div ref={bottomRef} />
      </div>
    </div>
  );
}
