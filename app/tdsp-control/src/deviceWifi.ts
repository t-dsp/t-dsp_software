// deviceWifi.ts — client for the ESP32 Wi-Fi settings API (projects/t-dsp_esp32_bt_receiver,
// WifiNetManager.h + the /api/wifi routes in main.cpp). Only exists on the WiFi control build.
//
// The device always runs its own network (festival mode) and also joins saved networks in range.
// This API lets the app add/forget those networks and rename/re-key the device network at runtime.
//
// Wire format is deliberately boring: GETs return JSON; POSTs send application/x-www-form-urlencoded
// with the device password in an `auth` field. No custom headers + a safelisted content type keeps
// every call a CORS "simple request", so it works cross-origin from a browser page (e.g. the jay-mint
// web host) without a preflight, as well as same-origin from the device-served page and from native.
//
// NOTE: no .web/.native siblings — safe to import as a runtime value from anywhere.

export interface WifiStatus {
  ap: { ssid: string; ip: string; clients: number; publicDefaultPass: boolean };
  sta: {
    state: 'idle' | 'waiting' | 'scanning' | 'connecting' | 'connected';
    ssid: string;
    ip?: string;
    rssi?: number;
    held: boolean;        // a scan is paused because a phone is on the device network
    nextScanS: number;
    scans: number;
    attempts: number;
    lastError: string;
  };
  saved: string[];
  maxSaved: number;
}

export interface ScanNet { ssid: string; rssi: number; secure: boolean; saved: boolean }
export interface ScanResults { state: 'idle' | 'running' | 'done'; ageS: number; results: ScanNet[] }
export interface ApiResult { ok: boolean; error?: string }

// Accepts what the Wi-Fi transport accepts ("tdsp.local", "192.168.4.1", "host:81", "ws://host:81/")
// and returns the device's HTTP origin (port 80), e.g. "http://192.168.4.1".
export function deviceHttpBase(target: string): string {
  let host = (target || '').trim() || 'tdsp.local';
  host = host.replace(/^wss?:\/\//i, '').replace(/^https?:\/\//i, '');
  host = host.split('/')[0];
  // Strip the (WebSocket) port: "[v6]:81" -> "[v6]", "host:81" -> "host". A bare v6 literal keeps its colons.
  host = host.startsWith('[') ? host.replace(/^(\[[^\]]*\]):\d+$/, '$1') : host.replace(/:\d+$/, '');
  return 'http://' + host;
}

const form = (f: Record<string, string>) =>
  Object.entries(f).map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');

async function request(url: string, init: RequestInit = {}, timeoutMs = 8000): Promise<Response> {
  const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
  const timer = setTimeout(() => ctl?.abort(), timeoutMs);
  try {
    return await fetch(url, { ...init, signal: ctl?.signal as any });
  } finally {
    clearTimeout(timer);
  }
}

async function getJson<T>(base: string, path: string): Promise<T> {
  const r = await request(base + path);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return (await r.json()) as T;
}

async function post(base: string, path: string, fields: Record<string, string>): Promise<ApiResult> {
  try {
    const r = await request(base + path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: form(fields),
    });
    const j = (await r.json().catch(() => null)) as ApiResult | null;
    if (j && typeof j.ok === 'boolean') return j;
    return { ok: r.ok, error: r.ok ? undefined : `HTTP ${r.status}` };
  } catch (e: any) {
    return { ok: false, error: e?.name === 'AbortError' ? 'device did not answer' : String(e?.message || e) };
  }
}

export function wifiApi(base: string) {
  return {
    status: () => getJson<WifiStatus>(base, '/api/wifi'),
    scanResults: () => getJson<ScanResults>(base, '/api/wifi/scan'),
    startScan: (auth: string) => post(base, '/api/wifi/scan', { auth }),
    save: (auth: string, ssid: string, pass: string) => post(base, '/api/wifi/save', { auth, ssid, pass }),
    forget: (auth: string, ssid: string) => post(base, '/api/wifi/forget', { auth, ssid }),
    connect: (auth: string) => post(base, '/api/wifi/connect', { auth }),
    setAp: (auth: string, ssid: string, pass: string) => post(base, '/api/wifi/ap', { auth, ssid, pass }),
  };
}

// One-line human summary of the station side, for the page and the Settings tile.
export function staSummary(st: WifiStatus): string {
  const s = st.sta;
  switch (s.state) {
    case 'connected': return `Joined ${s.ssid}`;
    case 'connecting': return `Joining ${s.ssid}…`;
    case 'scanning': return 'Looking for saved networks…';
    case 'idle': return 'Own network only';
    case 'waiting':
      if (s.held) return 'Search paused while a phone is connected';
      return st.saved.length ? `Not in range, next look in ${s.nextScanS} s` : 'Own network only';
  }
  return '';
}
