// loopXfer.ts — wire helpers for the note-editor clip round trip (@RECDUMP / @RECLOAD).
//
// PURE + sibling-free (no .web/.native twins, no platform APIs) so it is safe to import from
// BOTH platform transports — see the resolution note in transport.ts. base64 is hand-rolled
// (not atob/btoa/Buffer) precisely so this stays platform-agnostic across RN native + web.

// Raw bytes per @RD frame. 192 -> 256 base64 chars; the whole "@RD=<v>\x1f<seq>\x1f<b64>"
// line then fits the firmware's 288-byte control-line buffer with margin. Also a multiple of
// 8 (one LoopEvent) and 3 (no mid-stream base64 padding), which keeps frames tidy.
export const RD_RAW_CHUNK = 192;

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64INV: Record<string, number> = {};
for (let i = 0; i < B64.length; i++) B64INV[B64[i]] = i;

export function bytesToBase64(b: Uint8Array): string {
  let s = '';
  for (let i = 0; i < b.length; i += 3) {
    const n = (b[i] << 16) | ((i + 1 < b.length ? b[i + 1] : 0) << 8) | (i + 2 < b.length ? b[i + 2] : 0);
    s += B64[(n >> 18) & 63] + B64[(n >> 12) & 63]
      + (i + 1 < b.length ? B64[(n >> 6) & 63] : '=')
      + (i + 2 < b.length ? B64[n & 63] : '=');
  }
  return s;
}

export function base64ToBytes(s: string): Uint8Array {
  const clean = s.replace(/[^A-Za-z0-9+/]/g, '');
  const out = new Uint8Array((clean.length * 3) >> 2);
  let o = 0, acc = 0, bits = 0;
  for (let i = 0; i < clean.length; i++) {
    acc = (acc << 6) | B64INV[clean[i]];
    bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 0xff; }
  }
  return out.subarray(0, o);
}

// Build the @RD data frames for a load payload. The caller sends "@RECLOAD=<v>\x1f<bytes>"
// first (awaiting @RECOK), streams these frames (paced on BLE), then sends "@RECEND=<v>".
export function rdFrames(v: number, bytes: Uint8Array): string[] {
  const frames: string[] = [];
  let seq = 0;
  for (let off = 0; off < bytes.length; off += RD_RAW_CHUNK) {
    const chunk = bytes.subarray(off, Math.min(off + RD_RAW_CHUNK, bytes.length));
    frames.push(`@RD=${v}\x1f${seq++}\x1f${bytesToBase64(chunk)}`);
  }
  return frames;
}
