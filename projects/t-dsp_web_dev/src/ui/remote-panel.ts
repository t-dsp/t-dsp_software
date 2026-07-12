// Remote panel — in-app setup + QR for phone control.
//
// A floating button opens a modal where you enable remote control and
// enter the relay URL / device token / device id. Saving writes the
// config (via the preload bridge → main process) and the agent restarts
// in-process, so no app restart and no hand-edited JSON. When enabled, the
// modal also shows a QR of the relay's https URL to scan from the phone.
//
// Desktop-only: it relies on window.tdspCloud (electron/preload.js). In a
// plain browser (pnpm dev / static preview) it simply doesn't mount.

import qrcode from 'qrcode-generator';

interface CloudConfig {
  enabled: boolean;
  relayUrl: string;
  deviceToken: string;
  deviceId: string;
  envOverride?: boolean;
}
interface SetResult { ok: boolean; error?: string; effectiveRelayUrl?: string | null }

declare global {
  interface Window {
    tdspCloud?: {
      get(): Promise<CloudConfig>;
      set(cfg: Omit<CloudConfig, 'envOverride'>): Promise<SetResult>;
    };
  }
}

// wss://host  ->  https://host  (the phone opens the page over https)
function toHttps(url: string): string {
  return url.trim().replace(/^wss:/i, 'https:').replace(/^ws:/i, 'http:');
}

function qrDataUrl(url: string): string {
  const qr = qrcode(0, 'M');
  qr.addData(url);
  qr.make();
  return qr.createDataURL(8, 2);
}

export function mountRemotePanel(): void {
  const api = window.tdspCloud;
  if (!api) return; // not running in the Electron shell

  const style = document.createElement('style');
  style.textContent = `
    .rmt-btn { position: fixed; right: 16px; bottom: 16px; z-index: 9998;
      background: #232c3a; color: #e6edf3; border: 1px solid #3a4658;
      border-radius: 999px; padding: 10px 14px; font: 600 13px/1 system-ui, sans-serif;
      cursor: pointer; box-shadow: 0 4px 14px rgba(0,0,0,.4); }
    .rmt-btn:hover { background: #2c3850; }
    .rmt-btn .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%;
      background: #3a4252; margin-right: 7px; vertical-align: middle; }
    .rmt-btn .dot.on { background: #2ea043; }
    .rmt-overlay { position: fixed; inset: 0; z-index: 9999; display: none;
      align-items: center; justify-content: center; background: rgba(0,0,0,.6); }
    .rmt-overlay.open { display: flex; }
    .rmt-card { background: #151a23; border: 1px solid #222b38; border-radius: 16px;
      padding: 22px; width: 340px; color: #e6edf3; font-family: system-ui, sans-serif;
      box-shadow: 0 12px 40px rgba(0,0,0,.5); max-height: 90vh; overflow: auto; }
    .rmt-card h3 { margin: 0 0 14px; font-size: 16px; }
    .rmt-row { margin-bottom: 12px; }
    .rmt-row label { display: block; color: #7d8aa0; font-size: 12px; margin-bottom: 5px; }
    .rmt-row input[type=text] { width: 100%; padding: 10px; font-size: 14px; color: #e6edf3;
      background: #0e131b; border: 1px solid #222b38; border-radius: 9px; }
    .rmt-check { display: flex; align-items: center; gap: 9px; margin-bottom: 14px; }
    .rmt-check input { width: 18px; height: 18px; }
    .rmt-actions { display: flex; gap: 10px; align-items: center; margin-top: 4px; }
    .rmt-save { background: #4f8cff; color: #fff; border: none; border-radius: 9px;
      padding: 10px 18px; font: 600 13px/1 system-ui, sans-serif; cursor: pointer; }
    .rmt-close { background: #232c3a; color: #e6edf3; border: 1px solid #3a4658;
      border-radius: 9px; padding: 10px 16px; font: 600 13px/1 system-ui; cursor: pointer; }
    .rmt-status { font-size: 12px; color: #7d8aa0; margin-left: auto; }
    .rmt-status.ok { color: #2ea043; } .rmt-status.err { color: #f47067; }
    .rmt-qr { margin-top: 16px; padding-top: 16px; border-top: 1px solid #222b38; text-align: center; }
    .rmt-qr .cap { color: #7d8aa0; font-size: 12px; margin-bottom: 10px; }
    .rmt-qr .box { background: #fff; padding: 10px; border-radius: 10px; display: inline-block; }
    .rmt-qr img { display: block; width: 180px; height: 180px; image-rendering: pixelated; }
    .rmt-qr .url { margin-top: 10px; font: 11px/1.4 ui-monospace, Consolas, monospace;
      color: #9fb0c8; word-break: break-all; user-select: all; }
    .rmt-note { font-size: 11px; color: #7d8aa0; margin-top: 10px; }
  `;
  document.head.appendChild(style);

  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'rmt-btn';
  btn.innerHTML = '<span class="dot"></span>📱 Remote';

  const overlay = document.createElement('div');
  overlay.className = 'rmt-overlay';
  overlay.innerHTML = `
    <div class="rmt-card" role="dialog" aria-label="Remote control settings">
      <h3>Remote control</h3>
      <div class="rmt-check">
        <input type="checkbox" id="rmt-enabled" />
        <label for="rmt-enabled" style="margin:0;color:#e6edf3;font-size:14px">Enable remote control</label>
      </div>
      <div class="rmt-row"><label>Relay URL</label>
        <input type="text" id="rmt-url" placeholder="wss://your-app.up.railway.app" /></div>
      <div class="rmt-row"><label>Device token</label>
        <input type="text" id="rmt-token" placeholder="device token from the relay" /></div>
      <div class="rmt-row"><label>Device ID</label>
        <input type="text" id="rmt-id" placeholder="mixer-1" /></div>
      <div class="rmt-actions">
        <button class="rmt-save" type="button">Save</button>
        <button class="rmt-close" type="button">Close</button>
        <span class="rmt-status"></span>
      </div>
      <div class="rmt-note"></div>
      <div class="rmt-qr" style="display:none">
        <div class="cap">Scan with your phone camera</div>
        <div class="box"><img alt="QR code" /></div>
        <div class="url"></div>
      </div>
    </div>`;

  const $ = <T extends HTMLElement>(sel: string) => overlay.querySelector(sel) as T;
  const enabledEl = $<HTMLInputElement>('#rmt-enabled');
  const urlEl = $<HTMLInputElement>('#rmt-url');
  const tokenEl = $<HTMLInputElement>('#rmt-token');
  const idEl = $<HTMLInputElement>('#rmt-id');
  const statusEl = $<HTMLSpanElement>('.rmt-status');
  const noteEl = $<HTMLDivElement>('.rmt-note');
  const qrBox = $<HTMLDivElement>('.rmt-qr');
  const qrImg = $<HTMLImageElement>('.rmt-qr img');
  const qrUrl = $<HTMLDivElement>('.rmt-qr .url');
  const liveDot = btn.querySelector('.dot') as HTMLSpanElement;

  function refreshQr(): void {
    const url = urlEl.value.trim();
    if (enabledEl.checked && url) {
      const https = toHttps(url);
      qrImg.src = qrDataUrl(https);
      qrUrl.textContent = https;
      qrBox.style.display = '';
    } else {
      qrBox.style.display = 'none';
    }
  }

  function setStatus(text: string, kind: '' | 'ok' | 'err' = ''): void {
    statusEl.textContent = text;
    statusEl.className = 'rmt-status' + (kind ? ' ' + kind : '');
  }

  async function load(): Promise<void> {
    const c = await api!.get();
    enabledEl.checked = c.enabled;
    urlEl.value = c.relayUrl;
    tokenEl.value = c.deviceToken;
    idEl.value = c.deviceId || 'mixer-1';
    liveDot.classList.toggle('on', c.enabled && !!c.relayUrl && !!c.deviceToken);
    noteEl.textContent = c.envOverride
      ? 'Note: RELAY_URL/DEVICE_TOKEN env vars are set and override this file at launch.'
      : '';
    refreshQr();
  }

  async function save(): Promise<void> {
    setStatus('Saving…');
    const res = await api!.set({
      enabled: enabledEl.checked,
      relayUrl: urlEl.value.trim(),
      deviceToken: tokenEl.value.trim(),
      deviceId: idEl.value.trim() || 'mixer-1',
    });
    if (!res.ok) { setStatus(res.error || 'Save failed', 'err'); return; }
    setStatus(enabledEl.checked ? 'Saved — remote live ✓' : 'Saved — remote off', 'ok');
    liveDot.classList.toggle('on', enabledEl.checked && !!urlEl.value.trim() && !!tokenEl.value.trim());
    refreshQr();
  }

  const open = (o: boolean) => overlay.classList.toggle('open', o);
  btn.addEventListener('click', () => { void load(); open(true); });
  $('.rmt-close').addEventListener('click', () => open(false));
  $('.rmt-save').addEventListener('click', () => void save());
  enabledEl.addEventListener('change', refreshQr);
  urlEl.addEventListener('input', refreshQr);
  overlay.addEventListener('click', (e) => { if (e.target === overlay) open(false); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') open(false); });

  document.body.append(btn, overlay);
  void load(); // set the live dot + QR state on startup without opening
}
