import { decodeFlatBuffer } from '../transport/FbDecoder.js';

type WorkerInMsg = { type: 'init'; url: string };
type WorkerOutMsg =
  | { type: 'state'; data: Record<string, unknown> }
  | { type: 'status'; status: 'connected' | 'reconnecting' | 'disconnected'; attempt?: number }
  | { type: 'msg_count'; count: number };

const RECONNECT_MS = 2000;
let ws: WebSocket | null = null;
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
let attempt = 0;
let wsUrl = '';
let msgCount = 0;

setInterval(() => {
  if (msgCount > 0) {
    (self as unknown as Worker).postMessage({ type: 'msg_count', count: msgCount } satisfies WorkerOutMsg);
    msgCount = 0;
  }
}, 100);

function post(msg: WorkerOutMsg) {
  (self as unknown as Worker).postMessage(msg);
}

function connect(url: string) {
  wsUrl = url;
  
  // B2: Optional JSON fallback if 'format=json' is explicitly in URL or binary fails
  const sep = url.includes('?') ? '&' : '?';
  const forceJson = url.includes('format=json');
  const finalUrl = forceJson ? url : (url + sep + 'format=binary');
  
  ws = new WebSocket(finalUrl);
  if (!forceJson) ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    attempt = 0;
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    post({ type: 'status', status: 'connected' });
  };

  ws.onmessage = (e: MessageEvent) => {
    msgCount++;
    try {
      let data: Record<string, unknown>;
      if (e.data instanceof ArrayBuffer) {
        data = decodeFlatBuffer(new Uint8Array(e.data));
      } else {
        data = JSON.parse(e.data as string) as Record<string, unknown>;
      }
      post({ type: 'state', data });
    } catch (err) {
      // B1: log malformed frames instead of silent catch
      console.error('[WsWorker] Failed to decode frame:', err, e.data);
    }
  };

  ws.onclose = (e: CloseEvent) => {
    post({ type: 'status', status: 'disconnected' });
    
    // B7: Don't reconnect if unauthorized (401 handled by Http) or terminal close
    if (e.code === 1008 || e.code === 1011) {
      console.error('[WsWorker] Terminal closure:', e.code, e.reason);
      return;
    }

    attempt++;
    const delay = Math.min(30000, RECONNECT_MS * 2 ** (attempt - 1));
    console.log(`[WsWorker] Reconnecting in ${delay}ms (attempt ${attempt})...`);
    reconnectTimer = setTimeout(() => connect(wsUrl), delay);
    post({ type: 'status', status: 'reconnecting', attempt });
  };

  ws.onerror = (e) => {
    console.error('[WsWorker] WebSocket error:', e);
    ws?.close();
  };
}

(self as unknown as Worker).onmessage = (e: MessageEvent<WorkerInMsg>) => {
  if (e.data.type === 'init') connect(e.data.url);
};
