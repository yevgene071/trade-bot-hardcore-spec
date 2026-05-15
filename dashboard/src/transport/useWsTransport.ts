import { useEffect, useRef } from 'react';
import { useTradeStore } from '../store/useTradeStore';

const WS_URL = '/ws';
const RECONNECT_MS = 2000;

export function useWsTransport() {
  const applyServerState = useTradeStore(state => state.applyServerState);
  const setWsConnected = useTradeStore(state => state.setWsConnected);
  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    function connect() {
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const host = window.location.host;
      const url = `${protocol}//${host}${WS_URL}`;

      const ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen = () => {
        setWsConnected(true);
        if (reconnectTimer.current) {
          clearTimeout(reconnectTimer.current);
          reconnectTimer.current = null;
        }
      };

      ws.onmessage = (e) => {
        try {
          const data = JSON.parse(e.data);
          applyServerState(data);
        } catch {
          // ignore malformed frames
        }
      };

      ws.onclose = () => {
        setWsConnected(false);
        reconnectTimer.current = setTimeout(connect, RECONNECT_MS);
      };

      ws.onerror = () => {
        ws.close();
      };
    }

    connect();

    return () => {
      if (reconnectTimer.current) clearTimeout(reconnectTimer.current);
      wsRef.current?.close();
    };
  }, [applyServerState, setWsConnected]);
}
