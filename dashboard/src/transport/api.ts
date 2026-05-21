const BASE = '/api';

async function post(path: string, body?: unknown) {
  const res = await fetch(`${BASE}${path}`, {
    method: 'POST',
    headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!res.ok) throw new Error(`${path} failed: ${res.status}`);
  return res;
}

export async function apiSelectTicker(ticker: string) {
  return post('/ticker/select', { ticker });
}

export async function apiToggleKillSwitch() {
  return post('/killswitch/toggle');
}

export async function apiStartDump(filename?: string): Promise<{ ok: boolean; path?: string }> {
  const res = await post('/dump/start', { filename: filename || 'dump' });
  return res.json();
}

export async function apiStopDump() {
  return post('/dump/stop');
}

export async function apiSendCommand(command: string) {
  const res = await fetch(`${BASE}/command`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: command,
  });
  if (!res.ok) throw new Error(`/api/command failed: ${res.status}`);
  return res;
}

export async function apiGetLogs(n = 100): Promise<string[]> {
  const res = await fetch(`${BASE}/logs?n=${n}`);
  if (!res.ok) throw new Error(`/api/logs failed: ${res.status}`);
  return res.json();
}
