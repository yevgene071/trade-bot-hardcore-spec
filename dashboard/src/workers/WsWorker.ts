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
  const sep = url.includes('?') ? '&' : '?';
  ws = new WebSocket(url + sep + 'format=binary');
  ws.binaryType = 'arraybuffer';

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
    } catch {
      // ignore malformed frames
    }
  };

  ws.onclose = () => {
    post({ type: 'status', status: 'reconnecting', attempt: ++attempt });
    const delay = Math.min(30000, RECONNECT_MS * 2 ** (attempt - 1));
    reconnectTimer = setTimeout(() => connect(wsUrl), delay);
  };

  ws.onerror = () => ws?.close();
}

(self as unknown as Worker).onmessage = (e: MessageEvent<WorkerInMsg>) => {
  if (e.data.type === 'init') connect(e.data.url);
};
