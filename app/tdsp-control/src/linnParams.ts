// LinnStrument NRPN parameter table — drives the Settings › LinnStrument panel (LinnPanel.tsx).
// Numbers and ranges come from the device firmware's receivedNrpn() (rogerlinndesign/linnstrument-firmware,
// ls_midi.ino); the panel sends them through the box (`@LINN.SET=<n>,<v>`), which writes the NRPN over the
// USB-host port and re-reads the value so the UI shows what the device actually accepted.
// Per-split parameters are numbered for the LEFT split; the RIGHT split is the same number + 100.

export type LinnKind = 'enum' | 'bool' | 'int' | 'trigger' | 'note';
export interface LinnOpt { v: number; label: string }
export interface LinnParam {
  id: number;            // LEFT-split or global NRPN number
  name: string;
  kind: LinnKind;
  group: string;         // card the control lives in
  split?: boolean;       // per-split (RIGHT = id + 100)
  min?: number; max?: number;
  offset?: number;       // displayed = value + offset (octave / transpose)
  opts?: LinnOpt[];      // enum choices (sparse allowed)
  hint?: string;
}

export const RIGHT = 100;
export const splitId = (p: LinnParam, side: 0 | 1) => (p.split ? p.id + side * RIGHT : p.id);

const en = (labels: string[], from = 0): LinnOpt[] => labels.map((label, i) => ({ v: i + from, label }));
export const LINN_COLOURS = ['Default', 'Red', 'Yellow', 'Green', 'Cyan', 'Blue', 'Magenta', 'Off', 'White', 'Orange', 'Lime', 'Pink'];
const colours = (from: number) => en(LINN_COLOURS, 0).filter(o => o.v >= from);
const cc = (id: number, name: string, group: string, split = true, max = 127): LinnParam => ({ id, name, kind: 'int', group, split, min: 0, max });

export const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export const noteName = (n: number) => NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);

// ---- per-split --------------------------------------------------------------------------------
export const SPLIT_PARAMS: LinnParam[] = [
  { id: 0, name: 'MIDI mode', kind: 'enum', group: 'MIDI', split: true, opts: en(['One channel', 'Channel per note (MPE)', 'Channel per row']) },
  { id: 1, name: 'Main channel', kind: 'int', group: 'MIDI', split: true, min: 1, max: 16 },
  // 2..17 = per-note channels 1..16 enabled (rendered as a 16-toggle grid by the panel)
  { id: 18, name: 'Per-row lowest channel', kind: 'int', group: 'MIDI', split: true, min: 1, max: 16 },
  { id: 19, name: 'Bend range', kind: 'int', group: 'Pitch / X', split: true, min: 1, max: 96, hint: 'semitones each way; the panel offers 2 / 3 / 12 / 24' },
  { id: 20, name: 'Send pitch (X)', kind: 'bool', group: 'Pitch / X', split: true },
  { id: 21, name: 'Quantize', kind: 'bool', group: 'Pitch / X', split: true },
  { id: 22, name: 'Quantize hold', kind: 'enum', group: 'Pitch / X', split: true, opts: en(['Off', 'Medium', 'Fast', 'Slow']) },
  { id: 23, name: 'Reset pitch on release', kind: 'bool', group: 'Pitch / X', split: true },
  { id: 24, name: 'Send timbre (Y)', kind: 'bool', group: 'Timbre / Y', split: true },
  { id: 39, name: 'Y expression', kind: 'enum', group: 'Timbre / Y', split: true, opts: en(['Poly pressure', 'Channel pressure', 'CC (below)']) },
  cc(25, 'Y CC number', 'Timbre / Y'),
  { id: 26, name: 'Relative Y', kind: 'bool', group: 'Timbre / Y', split: true },
  cc(59, 'Relative Y initial value', 'Timbre / Y'),
  cc(54, 'Y minimum', 'Timbre / Y'), cc(55, 'Y maximum', 'Timbre / Y'),
  { id: 27, name: 'Send loudness (Z)', kind: 'bool', group: 'Loudness / Z', split: true },
  { id: 28, name: 'Z expression', kind: 'enum', group: 'Loudness / Z', split: true, opts: en(['Poly pressure', 'Channel pressure', 'CC (below)']) },
  cc(29, 'Z CC number', 'Loudness / Z'),
  { id: 58, name: 'Z as 14-bit CC', kind: 'bool', group: 'Loudness / Z', split: true },
  cc(56, 'Z minimum', 'Loudness / Z'), cc(57, 'Z maximum', 'Loudness / Z'),
  { id: 30, name: 'Main colour', kind: 'enum', group: 'Colours', split: true, opts: colours(1) },
  { id: 31, name: 'Accent colour', kind: 'enum', group: 'Colours', split: true, opts: colours(1) },
  { id: 32, name: 'Played colour', kind: 'enum', group: 'Colours', split: true, opts: colours(0) },
  { id: 33, name: 'Low row colour', kind: 'enum', group: 'Colours', split: true, opts: colours(1) },
  { id: 34, name: 'Low row mode', kind: 'enum', group: 'Low row', split: true, opts: en(['Normal', 'Restrike', 'Strum', 'Arpeggiator', 'Sustain', 'Bend', 'CC 1 (X)', 'CC 16-18 (XYZ)']) },
  { id: 48, name: 'Low row X behaviour', kind: 'enum', group: 'Low row', split: true, opts: en(['Hold', 'Fader']) },
  cc(49, 'Low row CC', 'Low row', true, 128),
  { id: 50, name: 'Low row XYZ behaviour', kind: 'enum', group: 'Low row', split: true, opts: en(['Hold', 'Fader']) },
  cc(51, 'Low row X CC', 'Low row', true, 128), cc(52, 'Low row Y CC', 'Low row', true, 128), cc(53, 'Low row Z CC', 'Low row', true, 128),
  { id: 35, name: 'Special', kind: 'enum', group: 'Special', split: true, opts: en(['Normal', 'Arpeggiator', 'CC faders', 'Strum', 'Sequencer']) },
  ...[0, 1, 2, 3, 4, 5, 6, 7].map(i => cc(40 + i, 'Fader ' + (i + 1) + ' CC', 'CC faders', true, 128)),
  { id: 36, name: 'Octave', kind: 'int', group: 'Octave / transpose', split: true, min: 0, max: 10, offset: -5 },
  { id: 37, name: 'Transpose pitch', kind: 'int', group: 'Octave / transpose', split: true, min: 0, max: 14, offset: -7 },
  { id: 38, name: 'Transpose lights', kind: 'int', group: 'Octave / transpose', split: true, min: 0, max: 14, offset: -7 },
  { id: 60, name: 'Channel-per-row order', kind: 'enum', group: 'MIDI', split: true, opts: en(['Normal', 'Reversed']) },
  { id: 61, name: 'Touch animation', kind: 'int', group: 'Colours', split: true, min: 0, max: 14 },
  { id: 62, name: 'Sequencer: play / stop', kind: 'trigger', group: 'Sequencer', split: true },
  { id: 63, name: 'Sequencer: previous pattern', kind: 'trigger', group: 'Sequencer', split: true },
  { id: 64, name: 'Sequencer: next pattern', kind: 'trigger', group: 'Sequencer', split: true },
  { id: 65, name: 'Sequencer: pattern', kind: 'enum', group: 'Sequencer', split: true, opts: en(['1', '2', '3', '4']) },
  { id: 66, name: 'Sequencer: mute', kind: 'trigger', group: 'Sequencer', split: true },
];
export const PER_NOTE_CH_BASE = 2;   // NRPN 2..17 = channel 1..16 enabled

// ---- global -------------------------------------------------------------------------------------
export const SWITCH_ASSIGN: LinnOpt[] = en(['Octave down', 'Octave up', 'Sustain', 'CC 65', 'Arpeggiator', 'Alt split', 'Auto octave', 'Tap tempo',
  'Legato', 'Latch', 'Preset up', 'Preset down', 'Reverse pitch X', 'Sequencer play', 'Sequencer prev', 'Sequencer next', 'MIDI clock', 'Sequencer mute']);
export const ROW_OFFSETS: LinnOpt[] = [{ v: 0, label: 'No overlap' }, { v: 3, label: '+3' }, { v: 4, label: '+4' }, { v: 5, label: '+5' }, { v: 6, label: '+6' }, { v: 7, label: '+7' },
  { v: 12, label: 'Octave' }, { v: 13, label: 'Guitar' }];

export const GLOBAL_PARAMS: LinnParam[] = [
  { id: 200, name: 'Split on', kind: 'bool', group: 'Split' },
  { id: 201, name: 'Selected split', kind: 'enum', group: 'Split', opts: en(['Left', 'Right']) },
  { id: 202, name: 'Split point column', kind: 'int', group: 'Split', min: 2, max: 25 },
  // 203..214 main note lights, 215..226 accent note lights (12-note toggle rows in the panel)
  { id: 247, name: 'Note lights preset', kind: 'int', group: 'Note lights', min: 0, max: 11 },
  { id: 227, name: 'Row offset', kind: 'enum', group: 'Row offset', opts: ROW_OFFSETS, hint: 'the device reports back what it accepted' },
  { id: 253, name: 'Custom row offset', kind: 'int', group: 'Row offset', min: 0, max: 33, offset: -17 },
  { id: 228, name: 'Switch 1', kind: 'enum', group: 'Switches', opts: SWITCH_ASSIGN },
  { id: 229, name: 'Switch 2', kind: 'enum', group: 'Switches', opts: SWITCH_ASSIGN },
  { id: 230, name: 'Foot switch left', kind: 'enum', group: 'Switches', opts: SWITCH_ASSIGN },
  { id: 231, name: 'Foot switch right', kind: 'enum', group: 'Switches', opts: SWITCH_ASSIGN },
  { id: 239, name: 'Switch 1 → both splits', kind: 'bool', group: 'Switches' },
  { id: 240, name: 'Switch 2 → both splits', kind: 'bool', group: 'Switches' },
  { id: 241, name: 'Foot left → both splits', kind: 'bool', group: 'Switches' },
  { id: 242, name: 'Foot right → both splits', kind: 'bool', group: 'Switches' },
  cc(257, 'Switch 1 CC (CC65 mode)', 'Switch CC numbers', false), cc(258, 'Switch 2 CC (CC65 mode)', 'Switch CC numbers', false),
  cc(255, 'Foot left CC (CC65 mode)', 'Switch CC numbers', false), cc(256, 'Foot right CC (CC65 mode)', 'Switch CC numbers', false),
  cc(261, 'Switch 1 sustain CC', 'Switch CC numbers', false), cc(262, 'Switch 2 sustain CC', 'Switch CC numbers', false),
  cc(259, 'Foot left sustain CC', 'Switch CC numbers', false), cc(260, 'Foot right sustain CC', 'Switch CC numbers', false),
  { id: 232, name: 'Velocity sensitivity', kind: 'enum', group: 'Velocity', opts: en(['Low', 'Medium', 'High', 'Fixed']) },
  { id: 249, name: 'Velocity minimum', kind: 'int', group: 'Velocity', min: 1, max: 127 },
  { id: 250, name: 'Velocity maximum', kind: 'int', group: 'Velocity', min: 1, max: 127 },
  { id: 251, name: 'Fixed velocity', kind: 'int', group: 'Velocity', min: 1, max: 127 },
  { id: 233, name: 'Pressure sensitivity', kind: 'enum', group: 'Pressure', opts: en(['Low', 'Medium', 'High']) },
  { id: 244, name: 'Pressure aftertouch', kind: 'bool', group: 'Pressure' },
  { id: 235, name: 'Arp direction', kind: 'enum', group: 'Arpeggiator', opts: en(['Up', 'Down', 'Up / down', 'Random', 'Replay all']) },
  { id: 236, name: 'Arp rate', kind: 'enum', group: 'Arpeggiator', opts: en(['1/8', '1/8 triplet', '1/16', '1/16 swing', '1/16 triplet', '1/32', '1/32 triplet'], 1) },
  { id: 237, name: 'Arp octaves', kind: 'enum', group: 'Arpeggiator', opts: en(['Off', '+1 octave', '+2 octaves']) },
  { id: 238, name: 'Clock BPM', kind: 'int', group: 'Clock', min: 1, max: 360 },
  { id: 234, name: 'MIDI I/O', kind: 'enum', group: 'MIDI I/O', opts: en(['MIDI jacks (DIN)', 'USB']), hint: 'must be USB for this panel to reach the device' },
  { id: 254, name: 'MIDI through', kind: 'bool', group: 'MIDI I/O' },
  { id: 252, name: 'Min µs between USB MIDI bytes', kind: 'int', group: 'MIDI I/O', min: 0, max: 512 },
  { id: 246, name: 'Left-handed', kind: 'bool', group: 'Misc' },
  { id: 245, name: 'User firmware mode', kind: 'bool', group: 'Misc', hint: 'hands the pads to a host program; the normal note layout stops' },
  ...[0, 1, 2, 3, 4, 5, 6, 7].map(i => ({ id: 263 + i, name: 'Row ' + (i + 1) + ' tuning', kind: 'note' as LinnKind, group: 'Guitar tuning', min: 0, max: 127 })),
];

export const NOTE_LIGHTS_MAIN = 203;    // +0..11 = C..B
export const NOTE_LIGHTS_ACCENT = 215;

export const groupsOf = (list: LinnParam[]) => { const out: string[] = []; for (const p of list) if (!out.includes(p.group)) out.push(p.group); return out; };

// ---- FACTORY DEFAULTS --------------------------------------------------------------------------
// From the device firmware's initializePresetSettings() / initializeMidiSettings() (ls_settings.ino):
// what a LinnStrument has after its own RESET action. Applied through the box (@LINN.APPLY=), so the
// device confirms every value. Not included: the clock BPM (238) and the USB byte interval (252).
// The CC-fader numbers (40-47) are the usual 1..8 — the firmware's table wasn't quoted.
export function factoryDefaults(): Record<number, number> {
  const v: Record<number, number> = {};
  for (const side of [0, 100]) {
    const right = side === 100;
    v[side + 0] = 0;                       // MIDI mode: one channel
    v[side + 1] = right ? 16 : 1;          // main channel
    for (let c = 1; c <= 16; c++) v[side + 1 + c] = (right ? (c >= 9 && c <= 15) : (c >= 2 && c <= 8)) ? 1 : 0;   // per-note channels
    v[side + 18] = right ? 9 : 1;          // per-row lowest channel
    v[side + 19] = 2;                      // bend range
    v[side + 20] = 1; v[side + 21] = 1; v[side + 22] = 1; v[side + 23] = 0;   // X: send, quantize, hold medium, no reset on release
    v[side + 24] = 1; v[side + 39] = 2; v[side + 25] = 74; v[side + 26] = 0; v[side + 59] = 64; v[side + 54] = 0; v[side + 55] = 127;   // Y: CC74
    v[side + 27] = 1; v[side + 28] = 0; v[side + 29] = 11; v[side + 58] = 0; v[side + 56] = 0; v[side + 57] = 127;   // Z: poly pressure
    v[side + 30] = right ? 5 : 3; v[side + 31] = 4; v[side + 32] = right ? 6 : 1; v[side + 33] = 2;   // colours
    v[side + 34] = 0; v[side + 48] = 0; v[side + 49] = 1; v[side + 50] = 0; v[side + 51] = 16; v[side + 52] = 17; v[side + 53] = 18;   // low row
    v[side + 35] = 0;                      // special: normal
    for (let i = 0; i < 8; i++) v[side + 40 + i] = 1 + i;   // CC faders
    v[side + 36] = 5; v[side + 37] = 7; v[side + 38] = 7;     // octave 0, transpose 0, lights 0
    v[side + 60] = 0; v[side + 61] = 0;    // row order normal, touch animation default
  }
  v[200] = 0; v[201] = 0; v[202] = 12;     // split off, left selected, split point 12
  const major = [0, 2, 4, 5, 7, 9, 11];
  for (let n = 0; n < 12; n++) { v[203 + n] = major.includes(n) ? 1 : 0; v[215 + n] = n === 0 ? 1 : 0; }   // note lights: C major, accent C
  v[227] = 5; v[253] = 12 + 17;            // row offset +5, custom offset 12
  v[228] = 2; v[229] = 4; v[230] = 4; v[231] = 2;   // switch 1 sustain, switch 2 arp, foot L arp, foot R sustain
  v[239] = v[240] = v[241] = v[242] = 0;
  v[248] = 65; v[255] = v[256] = v[257] = v[258] = 65; v[259] = v[260] = v[261] = v[262] = 64;
  v[232] = 1; v[249] = 1; v[250] = 127; v[251] = 96;   // velocity medium
  v[233] = 1; v[244] = 0;                  // pressure medium, no aftertouch
  v[235] = 4; v[236] = 4; v[237] = 0;      // arp: replay all, 1/16 swing, no octaves
  v[234] = 1; v[254] = 0;                  // MIDI I/O USB, through off
  v[245] = 0; v[246] = 0; v[247] = 0;
  [30, 35, 40, 45, 50, 55, 59, 64].forEach((n, i) => { v[263 + i] = n; });   // guitar tuning
  return v;
}
// Serialize a value map for @LINN.APPLY= (chunked so a line stays well under the lanes' limits).
export function applyChunks(values: Record<number, number>, maxLen = 1200): string[] {
  const out: string[] = []; let cur = '';
  for (const k of Object.keys(values).map(Number).sort((a, b) => a - b)) {
    const pair = k + ':' + values[k];
    if (cur.length + pair.length + 1 > maxLen) { out.push(cur); cur = ''; }
    cur += (cur ? ',' : '') + pair;
  }
  if (cur) out.push(cur);
  return out;
}
