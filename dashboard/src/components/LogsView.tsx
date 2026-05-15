import React, { useState, useEffect, useRef } from 'react';
import { apiGetLogs } from '../transport/api';
import { cn } from '../lib/utils';

export function LogsView() {
  const [logs, setLogs] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const bottomRef = useRef<HTMLDivElement>(null);

  const fetchLogs = async () => {
    setLoading(true);
    setError(null);
    try {
      const data = await apiGetLogs(200);
      setLogs(Array.isArray(data) ? data : []);
    } catch (e: any) {
      setError(e.message);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => { fetchLogs(); }, []);

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [logs]);

  return (
    <div className="h-full flex flex-col bg-black/40">
      <div className="p-3 border-b border-white/5 flex items-center justify-between bg-black/40 shrink-0">
        <div className="text-xs font-bold text-white uppercase tracking-wider">Bot Logs</div>
        <button
          onClick={fetchLogs}
          disabled={loading}
          className="px-3 py-1 text-[10px] font-bold uppercase tracking-wider rounded border border-white/10 text-[#7b859c] hover:text-white hover:border-white/30 transition-colors disabled:opacity-40"
        >
          {loading ? 'Loading...' : 'Refresh'}
        </button>
      </div>
      <div className="flex-1 overflow-y-auto no-scrollbar font-mono text-[11px] p-3 space-y-0.5">
        {error && (
          <div className="text-[#f43f5e] p-2 border border-[#f43f5e]/20 rounded bg-[#f43f5e]/5">
            Error: {error}
          </div>
        )}
        {logs.length === 0 && !loading && !error && (
          <div className="text-[#7b859c] text-center mt-10">No logs</div>
        )}
        {logs.map((line, i) => (
          <div key={i} className={cn(
            "leading-relaxed whitespace-pre-wrap break-all",
            line.includes('ERROR') ? "text-[#f43f5e]" :
            line.includes('WARN') ? "text-[#f59e0b]" :
            "text-[#7b859c] hover:text-[#c0c7d4]"
          )}>
            {line}
          </div>
        ))}
        <div ref={bottomRef} />
      </div>
    </div>
  );
}
