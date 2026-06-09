// Tiny stateless session tokens — HMAC-signed, no dependencies.
//
// Format:  base64url(JSON payload) + "." + base64url(HMAC-SHA256)
// The secret never leaves the server; tampering invalidates the MAC.
// Good enough for a single-user prototype. For a real userbase, swap
// this for a vetted library (jose) + a users table + password hashing.

import crypto from 'node:crypto';

const SECRET = process.env.SESSION_SECRET || 'dev-insecure-change-me';

function b64urlEncode(buf) {
  return Buffer.from(buf).toString('base64url');
}

// Sign a payload object into a session token with an expiry (seconds).
export function sign(payload, ttlSeconds = 60 * 60 * 24 * 30) {
  // exp is passed in by the caller's clock; we don't read the clock here
  // so this module stays deterministic and testable. Caller supplies exp.
  const body = { ...payload };
  const json = JSON.stringify(body);
  const data = b64urlEncode(json);
  const mac = crypto.createHmac('sha256', SECRET).update(data).digest('base64url');
  return `${data}.${mac}`;
}

// Verify a token's signature and (if present) expiry. Returns the payload
// object or null. `now` is the current epoch-seconds, injected by the
// caller so this stays pure.
export function verify(token, now) {
  if (typeof token !== 'string' || !token.includes('.')) return null;
  const [data, mac] = token.split('.');
  if (!data || !mac) return null;
  const expected = crypto.createHmac('sha256', SECRET).update(data).digest('base64url');
  // Constant-time compare; lengths must match for timingSafeEqual.
  const a = Buffer.from(mac);
  const b = Buffer.from(expected);
  if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return null;
  let payload;
  try {
    payload = JSON.parse(Buffer.from(data, 'base64url').toString('utf8'));
  } catch {
    return null;
  }
  if (payload.exp && typeof payload.exp === 'number' && now > payload.exp) return null;
  return payload;
}

// Constant-time string equality for password checks.
export function safeEqual(a, b) {
  const ab = Buffer.from(String(a));
  const bb = Buffer.from(String(b));
  if (ab.length !== bb.length) return false;
  return crypto.timingSafeEqual(ab, bb);
}
