// Minimal OSC 1.0 encoder + decoder.
//
// Supports the type tags T-DSP firmware actually emits and accepts:
//   i  int32   f  float32   s  string   b  blob
//   T  true    F  false     N  null     I  impulse
//
// Bundles are decoded recursively but never produced by this client.
// Pattern matching is not implemented — addresses are sent and matched literally.

export type OscArg = number | string | Uint8Array | boolean | null;

export interface OscMessage {
  address: string;
  types: string;
  args: OscArg[];
}

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder('utf-8');

const pad4 = (n: number): number => (4 - (n % 4)) % 4;

// ---------- encode ----------

export function encodeMessage(address: string, types: string, args: OscArg[]): Uint8Array {
  const parts: Uint8Array[] = [];
  parts.push(encodeString(address));
  parts.push(encodeString(',' + types));

  let argIdx = 0;
  for (const t of types) {
    switch (t) {
      case 'i':
        parts.push(encodeInt32(args[argIdx++] as number));
        break;
      case 'f':
        parts.push(encodeFloat32(args[argIdx++] as number));
        break;
      case 's':
        parts.push(encodeString(args[argIdx++] as string));
        break;
      case 'b':
        parts.push(encodeBlob(args[argIdx++] as Uint8Array));
        break;
      case 'T':
      case 'F':
      case 'N':
      case 'I':
        argIdx++;
        break;
      default:
        throw new Error(`unsupported OSC type tag '${t}'`);
    }
  }

  return concat(parts);
}

function encodeString(s: string): Uint8Array {
  const sb = textEncoder.encode(s);
  const totalUnpadded = sb.length + 1; // include null terminator
  const padded = totalUnpadded + pad4(totalUnpadded);
  const out = new Uint8Array(padded);
  out.set(sb, 0);
  // remaining bytes are zero (null terminator + padding)
  return out;
}

function encodeInt32(v: number): Uint8Array {
  const out = new Uint8Array(4);
  new DataView(out.buffer).setInt32(0, v | 0, false);
  return out;
}

function encodeFloat32(v: number): Uint8Array {
  const out = new Uint8Array(4);
  new DataView(out.buffer).setFloat32(0, v, false);
  return out;
}

function encodeBlob(b: Uint8Array): Uint8Array {
  const padded = b.length + pad4(b.length);
  const out = new Uint8Array(4 + padded);
  new DataView(out.buffer).setInt32(0, b.length, false);
  out.set(b, 4);
  return out;
}

function concat(arrs: Uint8Array[]): Uint8Array {
  let total = 0;
  for (const a of arrs) total += a.length;
  const out = new Uint8Array(total);
  let off = 0;
  for (const a of arrs) {
    out.set(a, off);
    off += a.length;
  }
  return out;
}

// ---------- decode ----------

export function decodePacket(data: Uint8Array): OscMessage[] {
  if (data.length >= 8) {
    const head = textDecoder.decode(data.subarray(0, 7));
    if (head === '#bundle' && data[7] === 0) {
      return decodeBundle(data);
    }
  }
  const m = decodeMessage(data);
  return m ? [m] : [];
}

function decodeBundle(data: Uint8Array): OscMessage[] {
  // 8 bytes "#bundle\0" + 8 bytes timetag = 16
  let off = 16;
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const out: OscMessage[] = [];
  while (off + 4 <= data.length) {
    const size = dv.getInt32(off, false);
    off += 4;
    if (off + size > data.length) break;
    out.push(...decodePacket(data.subarray(off, off + size)));
    off += size;
  }
  return out;
}

function decodeMessage(data: Uint8Array): OscMessage | null {
  if (data.length === 0) return null;
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);

  let off = 0;
  const [address, addrEnd] = readString(data, off);
  off = addrEnd;

  if (off >= data.length) {
    return { address, types: '', args: [] };
  }

  const [typeTag, typeEnd] = readString(data, off);
  off = typeEnd;
  if (!typeTag.startsWith(',')) return null;
  const types = typeTag.slice(1);

  const args: OscArg[] = [];
  for (const t of types) {
    switch (t) {
      case 'i':
        args.push(dv.getInt32(off, false));
        off += 4;
        break;
      case 'f':
        args.push(dv.getFloat32(off, false));
        off += 4;
        break;
      case 's': {
        const [s, end] = readString(data, off);
        args.push(s);
        off = end;
        break;
      }
      case 'b': {
        const len = dv.getInt32(off, false);
        off += 4;
        // copy out so the slice doesn't keep the whole frame buffer alive
        args.push(data.slice(off, off + len));
        off += len + pad4(len);
        break;
      }
      case 'T':
        args.push(true);
        break;
      case 'F':
        args.push(false);
        break;
      case 'N':
        args.push(null);
        break;
      case 'I':
        args.push(null);
        break;
      default:
        // unknown type — abort the message rather than mis-parse remaining args
        return { address, types, args };
    }
  }

  return { address, types, args };
}

function readString(data: Uint8Array, off: number): [string, number] {
  let end = off;
  while (end < data.length && data[end] !== 0) end++;
  const s = textDecoder.decode(data.subarray(off, end));
  const lenWithNull = end - off + 1;
  const padded = lenWithNull + pad4(lenWithNull);
  return [s, off + padded];
}
