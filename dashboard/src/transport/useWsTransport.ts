import { useEffect, useRef } from 'react';
import { useTradeStore } from '../store/useTradeStore';

export function useWsTransport() {
  const applyServerState = useTradeStore(state => state.applyServerState);
  const setWsStatus = useTradeStore(state => state.setWsStatus);
  const setReconnectAttempt = useTradeStore(state => state.setReconnectAttempt);
  const setLastReconnectAt = useTradeStore(state => state.setLastReconnectAt);
  const batchAddWsMsgCount = useTradeStore(state => state.batchAddWsMsgCount);
  const workerRef = useRef<Worker | null>(null);

  useEffect(() => {
    const worker = new Worker(
      new URL('../workers/WsWorker.ts', import.meta.url),
      { type: 'module' }
    );
    workerRef.current = worker;

    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${protocol}//${window.location.host}/ws`;
    worker.postMessage({ type: 'init', url });

    setWsStatus('connecting');

    worker.onmessage = (e) => {
      const msg = e.data as {
        type: 'state' | 'status' | 'msg_count';
        data?: Record<string, unknown>;
        status?: string;
        attempt?: number;
        count?: number;
      };
      switch (msg.type) {
        case 'state':
          if (msg.data) applyServerState(msg.data);
          break;
        case 'status':
          if (msg.status === 'connected') {
            setWsStatus('connected');
            setReconnectAttempt(0);
          } else if (msg.status === 'reconnecting') {
            setWsStatus('reconnecting');
            setLastReconnectAt(Date.now());
            setReconnectAttempt(msg.attempt ?? 1);
          } else {
            setWsStatus('disconnected');
          }
          break;
        case 'msg_count':
          batchAddWsMsgCount(msg.count ?? 0);
          break;
      }
    };

    return () => {
      worker.terminate();
      workerRef.current = null;
      setWsStatus('disconnected');
    };
  }, [applyServerState, setWsStatus, setReconnectAttempt, setLastReconnectAt, batchAddWsMsgCount]);
}
