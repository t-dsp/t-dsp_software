// arpSeq.ts — the user step-sequence model for the arpeggiator's PatUserSequence.
//
// This is a PURE module (no React, no platform-specific imports) so it is safe to
// import from BOTH the UI (ArpStepGrid.tsx) AND the platform-split transports
// (transport.web.ts / transport.native.ts). See the note in transport.ts: importing
// a *runtime* value from a module that has .web/.native siblings resolves back to the
// importing platform file, so shared runtime helpers must live in a sibling-free module
// like this one.
//
// A "user sequence" is the "actual arpeggiator preset" shape (à la Cthulhu / LibreArp /
// hardware arps): an explicit list of steps. Each step names WHICH held note plays — as a
// degree into the sorted-ascending held-note set — plus an octave shift and a velocity.
// Because the held notes are the raw material, one sequence transposes to whatever chord
// is held: press a C-minor triad and it plays the shape in C minor; press F and it follows.
//
// The wire format (one @ARPSEQ= line) mirrors the firmware parser in
// firmware/mix-kit/src/main.cpp: space-separated steps, each `degree[:octave[:velocity]]`,
// where degree is a number, `r` (rest) or `c` (chord = all held notes). octave defaults 0,
// velocity 0 = inherit the engine's VelMode.

export type SeqStep = {
  degree: number;   // >=0: index into the sorted-ascending held notes (wraps mod count)
  octave: number;   // octave shift applied to the emitted note [-3..+3]
  velocity: number; // 0 = inherit engine VelMode; else absolute 1..127
};

export const SEQ_REST = -1;   // silent step  (matches ArpFilter::SeqRest)
export const SEQ_CHORD = -2;  // all held notes (matches ArpFilter::SeqChord)

export const MAX_SEQ_STEPS = 32;   // ArpFilter::kMaxSteps
export const MAX_DEGREE = 7;       // UI caps the tap-cycle at the 8th held note, then chord

// The firmware Pattern enum index for PatUserSequence — the arp must be on THIS pattern
// for a loaded sequence to play. Keep in sync with lib/TDspArp/src/ArpFilter.h.
export const PAT_USER_SEQUENCE = 25;

// The full pattern list, index-aligned to the firmware `enum Pattern` (ArpFilter.h). The
// app selects a pattern by array index → @ARPPAT=<index>, so ORDER IS LOAD-BEARING: this
// must match the enum exactly. (This also fixes the old 4-item list that mislabeled
// everything past index 1.)
export const ARP_PATTERNS: string[] = [
  'Up', 'Down', 'Up/Down', 'Down/Up', 'Up/Down+', 'Down/Up+',
  'As Played', 'Played Rev', 'Random', 'Random Walk',
  'Chord', 'Chord+Up', 'Chord Stab', 'Converge', 'Diverge',
  'Thumb', 'Thumb U/D', 'Pinky', 'Pinky U/D',
  'Asc 3', 'Asc 4', 'Crab Walk', 'Stair', 'Spiral', 'Euclidean',
  'User Seq',   // index 25 = PAT_USER_SEQUENCE
];

// Rate pills shown in the app, each mapped to its firmware `enum Rate` index (ArpFilter.h).
// The app shows a curated 6 of the 15 firmware rates; `fw` is what @ARPRATE= expects.
// (Previously the app sent the ARRAY index, so every rate was wrong — e.g. "1/4" sent 0 =
// whole note. Sending `.fw` fixes that and lets a preset's firmware rate map back to a pill.)
export type ArpRate = { label: string; fw: number };
export const ARP_RATES: ArpRate[] = [
  { label: '1/4', fw: 5 }, { label: '1/8', fw: 8 }, { label: '1/8T', fw: 9 },
  { label: '1/16', fw: 11 }, { label: '1/16T', fw: 12 }, { label: '1/32', fw: 13 },
];
// Map a firmware Rate index to the nearest app pill index (defaults to 1/16 if off-list).
export const rateIndexFromFw = (fw: number): number => {
  const i = ARP_RATES.findIndex(r => r.fw === fw);
  return i < 0 ? 3 : i;
};

// Encode one step into its wire token. Only emit the octave/velocity fields when they
// carry information, keeping the @ARPSEQ line short (and well under the device line buffer).
function encodeStep(st: SeqStep): string {
  const d = st.degree === SEQ_REST ? 'r' : st.degree === SEQ_CHORD ? 'c' : String(st.degree | 0);
  if (st.velocity) return `${d}:${st.octave | 0}:${st.velocity | 0}`;
  if (st.octave)   return `${d}:${st.octave | 0}`;
  return d;
}

// Build the @ARPSEQ= payload (without the "@ARPSEQ=" prefix) from a step list.
export function encodeSequence(steps: SeqStep[]): string {
  return steps.slice(0, MAX_SEQ_STEPS).map(encodeStep).join(' ');
}

// A full arp parameter bundle for the atomic @ARPPRESET= command. All fields optional —
// only the ones present are emitted (and applied on the device), so a caller can send a
// subset. Keys/units mirror the firmware handler in firmware/mix-kit/src/main.cpp.
export type ArpWireParams = {
  pat?: number; rate?: number;      // firmware Pattern / Rate enum indices
  gatePct?: number; swingPct?: number;  // percents (gate 5..150, swing 50..85)
  oct?: number; octMode?: number; latch?: boolean;
  velMode?: number; velFixed?: number; velAccent?: number;
  stepMask?: number; stepLength?: number; mpeMode?: number;
  outCh?: number; scatterBase?: number; scatterCount?: number;
  scale?: number; scaleRoot?: number; transpose?: number; repeat?: number;
};

// Build the @ARPPRESET= payload (space-separated key=value tokens) from a param bundle.
// The key strings must match the firmware parser exactly.
export function encodeArpParams(p: ArpWireParams): string {
  const t: string[] = [];
  const add = (k: string, v: number | undefined) => { if (v !== undefined) t.push(`${k}=${v | 0}`); };
  add('pat', p.pat);           add('rate', p.rate);
  add('gate', p.gatePct);      add('swing', p.swingPct);
  add('oct', p.oct);           add('octm', p.octMode);
  if (p.latch !== undefined) t.push(`latch=${p.latch ? 1 : 0}`);
  add('vel', p.velMode);       add('vfix', p.velFixed);   add('vacc', p.velAccent);
  add('mask', p.stepMask);     add('len', p.stepLength);  add('mpe', p.mpeMode);
  add('outch', p.outCh);       add('scb', p.scatterBase); add('scc', p.scatterCount);
  add('scale', p.scale);       add('scroot', p.scaleRoot);
  add('tr', p.transpose);      add('rep', p.repeat);
  return t.join(' ');
}

// Convenience step constructors (keep call sites terse in the SHAPES table below).
const n = (degree: number, octave = 0, velocity = 0): SeqStep => ({ degree, octave, velocity });
const R = n(SEQ_REST);
const C = (octave = 0, velocity = 0): SeqStep => n(SEQ_CHORD, octave, velocity);

const ACCENT = 112;   // downbeat velocity used by the shipped shapes
const GHOST  = 64;    // quiet in-between step

// Built-in "shape" presets — the prebuilt arpeggiator patterns the user picks and then
// tweaks. These are the app's canonical set; lib/TDspArp/presets.json carries a matching
// "sequence" category as the firmware-library reference. Each is <= 16 steps so the
// encoded @ARPSEQ line stays compact.
export type ArpShape = { name: string; steps: SeqStep[] };
export const SHAPES: ArpShape[] = [
  { name: 'Up 8',        steps: [n(0, 0, ACCENT), n(1), n(2), n(3), n(4), n(5), n(6), n(7)] },
  { name: 'Up + Octave', steps: [n(0, 0, ACCENT), n(1), n(2), n(3), n(0, 1), n(1, 1), n(2, 1), n(3, 1)] },
  { name: 'Down 8',      steps: [n(7, 0, ACCENT), n(6), n(5), n(4), n(3), n(2), n(1), n(0)] },
  { name: 'Octave Jump', steps: [n(0, 0, ACCENT), n(0, 1), n(0, 2), n(0, 1)] },
  { name: 'Trance Gate', steps: [n(0, 0, ACCENT), R, n(0, 0, GHOST), n(0), R, n(0, 0, ACCENT), R, n(0, 0, GHOST)] },
  { name: 'Gallop',      steps: [n(0, 0, ACCENT), n(0, 0, GHOST), n(1), R, n(0, 0, ACCENT), n(0, 0, GHOST), n(2), R] },
  { name: 'Chord Stab',  steps: [C(0, ACCENT), R, C(0, GHOST), R, C(0, ACCENT), R, C(1), C(0, GHOST)] },
  { name: 'Pedal Thumb', steps: [n(0, 0, ACCENT), n(1), n(0), n(2), n(0, 0, ACCENT), n(3), n(0), n(4)] },
  { name: 'Bounce',      steps: [n(0, 0, ACCENT), n(2), n(1), n(3)] },
  { name: 'Stair',       steps: [n(0, 0, ACCENT), n(0), n(1), n(1), n(2), n(2), n(3), n(3)] },
];

export const DEFAULT_SHAPE = SHAPES[0];
