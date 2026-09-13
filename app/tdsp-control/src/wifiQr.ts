// wifiQr.ts — pure Wi-Fi QR helpers (no React Native), shared by ui/WifiQr.tsx and tests.
import qrcode from 'qrcode-generator';

// Standard Wi-Fi QR payload: WIFI:T:WPA;S:<ssid>;P:<password>;;  with \ ; , : " escaped.
export function wifiQrPayload(ssid: string, pass: string): string {
  const esc = (s: string) => s.replace(/([\\;,:"])/g, '\\$1');
  return pass ? `WIFI:T:WPA;S:${esc(ssid)};P:${esc(pass)};;` : `WIFI:T:nopass;S:${esc(ssid)};;`;
}

// Module matrix as runs of equal colour per row, so a ~33x33 code is a few hundred Views, not ~1,100.
export function qrRuns(value: string): { n: number; rows: [boolean, number][][] } {
  const utf8 = (qrcode as any).stringToBytesFuncs?.['UTF-8'];
  if (utf8) (qrcode as any).stringToBytes = utf8;   // SSIDs/passwords may be non-ASCII
  const qr = qrcode(0, 'M');
  qr.addData(value);
  qr.make();
  const n = qr.getModuleCount();
  const rows: [boolean, number][][] = [];
  for (let r = 0; r < n; r++) {
    const runs: [boolean, number][] = [];
    for (let c = 0; c < n;) {
      const dark = qr.isDark(r, c);
      let len = 1;
      while (c + len < n && qr.isDark(r, c + len) === dark) len++;
      runs.push([dark, len]);
      c += len;
    }
    rows.push(runs);
  }
  return { n, rows };
}
