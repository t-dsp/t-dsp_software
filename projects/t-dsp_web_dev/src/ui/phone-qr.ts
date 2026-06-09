// Phone-QR widget — "scan to control from your phone".
//
// Self-contained DOM overlay (no panel-system or framework coupling): a
// small floating button that toggles a card showing a QR code of the cloud
// relay URL plus the address as selectable text. Mounted only when the
// Electron main process passes a `?remote=<https-url>` query param, i.e.
// when remote control is actually configured (see electron/main.js).

import qrcode from 'qrcode-generator';

export function mountPhoneQr(url: string): void {
  // Inject styles once.
  const style = document.createElement('style');
  style.textContent = `
    .pqr-btn { position: fixed; right: 16px; bottom: 16px; z-index: 9998;
      background: #232c3a; color: #e6edf3; border: 1px solid #3a4658;
      border-radius: 999px; padding: 10px 14px; font: 600 13px/1 system-ui, sans-serif;
      cursor: pointer; box-shadow: 0 4px 14px rgba(0,0,0,.4); }
    .pqr-btn:hover { background: #2c3850; }
    .pqr-overlay { position: fixed; inset: 0; z-index: 9999; display: none;
      align-items: center; justify-content: center; background: rgba(0,0,0,.6); }
    .pqr-overlay.open { display: flex; }
    .pqr-card { background: #151a23; border: 1px solid #222b38; border-radius: 16px;
      padding: 22px; width: 300px; text-align: center; color: #e6edf3;
      font-family: system-ui, sans-serif; box-shadow: 0 12px 40px rgba(0,0,0,.5); }
    .pqr-card h3 { margin: 0 0 4px; font-size: 16px; }
    .pqr-card .sub { color: #7d8aa0; font-size: 12px; margin-bottom: 16px; }
    .pqr-qr { background: #fff; padding: 12px; border-radius: 10px; display: inline-block; }
    .pqr-qr img { display: block; width: 220px; height: 220px; image-rendering: pixelated; }
    .pqr-url { margin: 14px 0 16px; font: 12px/1.4 ui-monospace, Consolas, monospace;
      color: #9fb0c8; word-break: break-all; user-select: all; }
    .pqr-close { background: #4f8cff; color: #fff; border: none; border-radius: 9px;
      padding: 9px 18px; font: 600 13px/1 system-ui, sans-serif; cursor: pointer; }
  `;
  document.head.appendChild(style);

  // QR image (GIF data URL, rendered crisp).
  const qr = qrcode(0, 'M');
  qr.addData(url);
  qr.make();
  const imgSrc = qr.createDataURL(8, 2);

  const btn = document.createElement('button');
  btn.className = 'pqr-btn';
  btn.type = 'button';
  btn.textContent = '📱 Phone';
  btn.title = 'Show QR to control from your phone';

  const overlay = document.createElement('div');
  overlay.className = 'pqr-overlay';
  overlay.innerHTML = `
    <div class="pqr-card" role="dialog" aria-label="Control from your phone">
      <h3>Control from your phone</h3>
      <div class="sub">Scan with your phone camera</div>
      <div class="pqr-qr"><img alt="QR code" src="${imgSrc}" /></div>
      <div class="pqr-url">${url}</div>
      <button class="pqr-close" type="button">Close</button>
    </div>`;

  const open = (o: boolean) => overlay.classList.toggle('open', o);
  btn.addEventListener('click', () => open(true));
  overlay.addEventListener('click', (e) => { if (e.target === overlay) open(false); });
  overlay.querySelector('.pqr-close')!.addEventListener('click', () => open(false));
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') open(false); });

  document.body.append(btn, overlay);
}
