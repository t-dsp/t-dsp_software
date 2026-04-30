// biquad-design.ts — RBJ audio EQ cookbook biquad designers + magnitude
// response evaluator, mirroring lib/TAC5212/src/TAC5212_Biquad.cpp.
//
// Pure functions; no DOM, no I/O. Used by the biquad widget to:
//   1. Compute on-chip 5.27 fixed-point coefficients from (type, freq, gainDb, Q)
//      so the dev surface can ship `<base>/coeffs iiiii` in the same format
//      the firmware accepts.
//   2. Evaluate the cascade's magnitude response (in dB) on a frequency
//      grid for the live curve preview.
//
// Storage convention matches the firmware:
//   N0 = b0,  N1 = b1 / 2,  N2 = b2,  D1 = -a1 / 2,  D2 = -a2
// where (b0,b1,b2,a0,a1,a2) is the textbook biquad with a0 normalized to 1.

import type { BiquadType } from './codec-panel-config';

export interface BiquadCoeffs {
  // Signed 32-bit register values (5.27 fixed-point).
  n0: number;
  n1: number;
  n2: number;
  d1: number;
  d2: number;
}

export const BYPASS_COEFFS: BiquadCoeffs = {
  n0: 0x7FFFFFFF, n1: 0, n2: 0, d1: 0, d2: 0,
};

const TWO_PI = Math.PI * 2;

// Coefficient encoding. The TAC5212 expects unity at full int32 scale
// (≈2³¹) — see BiquadCoeffs::bypass() in TAC5212.h, which uses 0x7FFFFFFF
// for N0. Older 5.27 framing is misleading; the actual unity reference
// matches the chip's denominator constant of 2³¹.
function toQ27(coef: number): number {
  const scale = 2 ** 31;
  const scaled = coef * scale;
  if (scaled >= 2147483647) return 0x7FFFFFFF;
  if (scaled < -2147483648) return -0x80000000;
  return Math.trunc(scaled) | 0;
}

function fromQ27(reg: number): number {
  const invScale = 1 / (2 ** 31);
  const signed = reg | 0;
  return signed * invScale;
}

function pack(b0: number, b1: number, b2: number,
              a0: number, a1: number, a2: number): BiquadCoeffs {
  const inv_a0 = 1 / a0;
  b0 *= inv_a0;
  b1 *= inv_a0;
  b2 *= inv_a0;
  a1 *= inv_a0;
  a2 *= inv_a0;
  return {
    n0: toQ27(b0),
    n1: toQ27(b1 * 0.5),
    n2: toQ27(b2),
    d1: toQ27(-a1 * 0.5),
    d2: toQ27(-a2),
  };
}

export function bqBypass(): BiquadCoeffs {
  return { ...BYPASS_COEFFS };
}

export function bqPeak(fs: number, fc: number, dB: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const A = Math.pow(10, dB / 40);
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);

  const b0 =  1 + alpha * A;
  const b1 = -2 * cosw0;
  const b2 =  1 - alpha * A;
  const a0 =  1 + alpha / A;
  const a1 = -2 * cosw0;
  const a2 =  1 - alpha / A;
  return pack(b0, b1, b2, a0, a1, a2);
}

export function bqLowShelf(fs: number, fc: number, dB: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const A = Math.pow(10, dB / 40);
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);
  const beta = 2 * Math.sqrt(A) * alpha;

  const b0 =       A * ((A + 1) - (A - 1) * cosw0 + beta);
  const b1 =   2 * A * ((A - 1) - (A + 1) * cosw0);
  const b2 =       A * ((A + 1) - (A - 1) * cosw0 - beta);
  const a0 =           (A + 1) + (A - 1) * cosw0 + beta;
  const a1 =     - 2 * ((A - 1) + (A + 1) * cosw0);
  const a2 =           (A + 1) + (A - 1) * cosw0 - beta;
  return pack(b0, b1, b2, a0, a1, a2);
}

export function bqHighShelf(fs: number, fc: number, dB: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const A = Math.pow(10, dB / 40);
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);
  const beta = 2 * Math.sqrt(A) * alpha;

  const b0 =       A * ((A + 1) + (A - 1) * cosw0 + beta);
  const b1 =  -2 * A * ((A - 1) + (A + 1) * cosw0);
  const b2 =       A * ((A + 1) + (A - 1) * cosw0 - beta);
  const a0 =           (A + 1) - (A - 1) * cosw0 + beta;
  const a1 =       2 * ((A - 1) - (A + 1) * cosw0);
  const a2 =           (A + 1) - (A - 1) * cosw0 - beta;
  return pack(b0, b1, b2, a0, a1, a2);
}

export function bqLowpass(fs: number, fc: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);

  const b0 = (1 - cosw0) / 2;
  const b1 =  1 - cosw0;
  const b2 = (1 - cosw0) / 2;
  const a0 =  1 + alpha;
  const a1 = -2 * cosw0;
  const a2 =  1 - alpha;
  return pack(b0, b1, b2, a0, a1, a2);
}

export function bqHighpass(fs: number, fc: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);

  const b0 =  (1 + cosw0) / 2;
  const b1 = -(1 + cosw0);
  const b2 =  (1 + cosw0) / 2;
  const a0 =   1 + alpha;
  const a1 =  -2 * cosw0;
  const a2 =   1 - alpha;
  return pack(b0, b1, b2, a0, a1, a2);
}

export function bqBandpass(fs: number, fc: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);
  const b0 =  alpha;
  const b1 =  0;
  const b2 = -alpha;
  const a0 =  1 + alpha;
  const a1 = -2 * cosw0;
  const a2 =  1 - alpha;
  return pack(b0, b1, b2, a0, a1, a2);
}

export function bqNotch(fs: number, fc: number, Q: number): BiquadCoeffs {
  if (fs <= 0 || fc <= 0 || Q <= 0) return bqBypass();
  const w0 = TWO_PI * fc / fs;
  const cosw0 = Math.cos(w0);
  const sinw0 = Math.sin(w0);
  const alpha = sinw0 / (2 * Q);
  const b0 =  1;
  const b1 = -2 * cosw0;
  const b2 =  1;
  const a0 =  1 + alpha;
  const a1 = -2 * cosw0;
  const a2 =  1 - alpha;
  return pack(b0, b1, b2, a0, a1, a2);
}

// Top-level dispatcher used by the widget: pick the right designer for the
// chosen type. `gainDb` and `Q` are present in all signatures even if the
// designer ignores one of them; the widget greys out unused sliders per
// type.
export function design(type: BiquadType,
                       fs: number, fc: number, gainDb: number, Q: number): BiquadCoeffs {
  switch (type) {
    case 'off':        return bqBypass();
    case 'peak':       return bqPeak(fs, fc, gainDb, Q);
    case 'low_shelf':  return bqLowShelf(fs, fc, gainDb, Q);
    case 'high_shelf': return bqHighShelf(fs, fc, gainDb, Q);
    case 'low_pass':   return bqLowpass(fs, fc, Q);
    case 'high_pass':  return bqHighpass(fs, fc, Q);
    case 'band_pass':  return bqBandpass(fs, fc, Q);
    case 'notch':      return bqNotch(fs, fc, Q);
  }
}

// Magnitude response. Returns linear gain at frequency `f`.
export function magnitude(c: BiquadCoeffs, fs: number, f: number): number {
  if (fs <= 0 || f <= 0) return 1;
  // Recover textbook coefs from storage form.
  const b0 = fromQ27(c.n0);
  const b1 = fromQ27(c.n1) * 2;
  const b2 = fromQ27(c.n2);
  const a1 = -fromQ27(c.d1) * 2;
  const a2 = -fromQ27(c.d2);

  const w = TWO_PI * f / fs;
  const cw = Math.cos(w);
  const sw = Math.sin(w);
  const c2w = Math.cos(2 * w);
  const s2w = Math.sin(2 * w);

  const numRe = b0 + b1 * cw + b2 * c2w;
  const numIm =     -b1 * sw - b2 * s2w;
  const denRe = 1 + a1 * cw + a2 * c2w;
  const denIm =    -a1 * sw - a2 * s2w;

  const numMag = Math.hypot(numRe, numIm);
  const denMag = Math.hypot(denRe, denIm);
  if (denMag <= 1e-30) return 0;
  return numMag / denMag;
}

export function magnitudeDb(c: BiquadCoeffs, fs: number, f: number): number {
  const m = magnitude(c, fs, f);
  if (m <= 1e-10) return -200;
  return 20 * Math.log10(m);
}

// Cascade magnitude (sum in dB) for the response curve.
export function magnitudeDbCascade(coefs: readonly BiquadCoeffs[],
                                   fs: number, f: number): number {
  let sum = 0;
  for (const c of coefs) sum += magnitudeDb(c, fs, f);
  return sum;
}

// True-int32 equality (for snapshot round-tripping) — JS bitwise ops
// behave as signed 32-bit so this works.
export function coeffsEqual(a: BiquadCoeffs, b: BiquadCoeffs): boolean {
  return ((a.n0 | 0) === (b.n0 | 0)) &&
         ((a.n1 | 0) === (b.n1 | 0)) &&
         ((a.n2 | 0) === (b.n2 | 0)) &&
         ((a.d1 | 0) === (b.d1 | 0)) &&
         ((a.d2 | 0) === (b.d2 | 0));
}
