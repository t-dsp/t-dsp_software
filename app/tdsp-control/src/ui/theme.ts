// theme.ts — the T-DSP control surface's color system + a couple of shared layout constants.
// Extracted from App.tsx (Phase 1 split) so the palette, per-section themes, and the styles
// that consume them can live apart from the component tree. No React/RN imports — pure values,
// safe to import from anywhere (styles, primitives, App) with no cycle risk.

export const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', accent2: '#a371f7', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };

// Per-section theme: a title/border accent + a translucent card background (over the dark app bg),
// so each area reads as its own color and a submenu's sub-cards inherit the parent's tint. Each is
// { accent, tint } — accent tints the title/left-border; tint is the see-through card fill.
const th = (accent: string, a: number) => ({ accent, tint: accent + Math.round(a * 255).toString(16).padStart(2, '0') });
export const THEME = {
  synthA:   th('#3fb950', 0.14),   // green
  synthB:   th('#a371f7', 0.15),   // purple
  synthC:   th('#2dd4bf', 0.14),   // teal (voice 3, 4-voice pool)
  synthD:   th('#e3b341', 0.14),   // gold (voice 4)
  tempo:    th('#e3b341', 0.14),   // amber
  bt:       th('#58a6ff', 0.14),   // blue
  usb:      th('#79c0ff', 0.14),   // light blue (USB audio interface)
  settings: th('#ff7b72', 0.13),   // coral
  recorder:  th('#f85149', 0.14),  // red (MIDI record)
  audioloop: th('#f778ba', 0.14),  // pink (audio loop)
  drums:     th('#f0883e', 0.14),  // orange (drum track)
};
export const HDR_H = 38;   // shared height for page-header control buttons (back / keyboard / transport) so they line up
export const DOWNBEAT = '#e3b341';   // amber — the accented beat 1, distinct from the green beats
