// Approximate fader-position-to-dB display.
//
// Real X32 fader curves are piecewise (different slopes in different regions).
// This is a single-segment approximation good enough for an at-the-bench
// "is the fader doing roughly what I expect" reading. If the curve ever needs
// to match X32 exactly, replace with the piecewise mapping.

export function formatFaderDb(v: number): string {
  if (v <= 0.0005) return '-∞';
  // 0.75 ≈ 0 dB, 1.0 ≈ +10 dB, 0.001 ≈ -60 dB-ish
  const db = 20 * Math.log10(v) + 6.0;
  return `${db.toFixed(1)} dB`;
}
