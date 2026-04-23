// Beats presets — click-to-load rhythm templates.
//
// Each preset is a pattern + BPM + swing. Applying a preset clears
// every track and re-populates just the "on" cells; the firmware's
// 3 ms OSC frame throttle spaces out the writes so no frames are
// dropped even for a full 64-cell pattern.
//
// Pattern shape: 4 tracks × 16 steps, boolean.
//   Track 0 = Kick   (AudioSynthSimpleDrum)
//   Track 1 = Snare  (AudioSynthSimpleDrum)
//   Track 2 = Hat    (AudioPlaySdWav — needs HAT.WAV on SD to be audible)
//   Track 3 = Perc   (AudioPlaySdWav — needs CLAP.WAV or override)
//
// When `swing > 0` the sequencer lengthens even-index steps and shortens
// odd-index steps (MPC-style), producing the classic "boom .. tick .. boom ..
// tick" feel. D&B presets keep swing at 0 ("straight") per the user's ask.

export interface BeatsPreset {
  id:      string;
  name:    string;
  bpm:     number;
  swing:   number;
  pattern: boolean[][];  // 4 tracks × 16 steps
}

// Helper: turn a "x..x.x.." string into a boolean[16]. Any non-'.' char
// counts as on, so "x", "X", "1" all work. Keeps preset definitions
// readable vs. raw boolean arrays.
const row = (s: string): boolean[] => {
  const out: boolean[] = [];
  for (let i = 0; i < 16; i++) out.push((s[i] ?? '.') !== '.');
  return out;
};

export const BEATS_PRESETS: BeatsPreset[] = [
  // ---- 8 general-style presets ----

  {
    id: 'four-on-floor',
    name: 'Four/Floor',
    bpm: 124,
    swing: 0,
    // Classic house: kick every quarter, snare on 2 and 4,
    // offbeat hats. The default preload on cold boot.
    pattern: [
      row('x...x...x...x...'),
      row('....x.......x...'),
      row('..x...x...x...x.'),
      row('................'),
    ],
  },
  {
    id: 'boom-bap',
    name: 'Boom Bap',
    bpm: 90,
    swing: 0.15,
    // Hip-hop: kick on 1 and "and-of-3" with a ghost on "and-of-4",
    // snare on 2 and 4, straight 8th hats. Swing adds the head-nod.
    pattern: [
      row('x.......x.x.....'),
      row('....x.......x...'),
      row('x.x.x.x.x.x.x.x.'),
      row('................'),
    ],
  },
  {
    id: 'trap',
    name: 'Trap',
    bpm: 140,
    swing: 0,
    // Trap: syncopated kick + backbeat snare + 32nd-note hat rolls
    // approximated by doubled-up hats on the "and-a" beats.
    pattern: [
      row('x.....x.x.......'),
      row('....x.......x...'),
      row('x.x.x.xxx.x.x.xx'),
      row('................'),
    ],
  },
  {
    id: 'breakbeat',
    name: 'Breakbeat',
    bpm: 135,
    swing: 0,
    // Breakbeat: kick on 1 and 9, snare on 5 and 13, busy hats,
    // ghost perc. Amen-adjacent but simpler.
    pattern: [
      row('x.......x.......'),
      row('....x.......x...'),
      row('x.x.x.x.x.x.x.x.'),
      row('......x.....x...'),
    ],
  },
  {
    id: 'funk',
    name: 'Funk',
    bpm: 105,
    swing: 0.18,
    // Funk: kick syncopation around the snare, ghost snares, steady
    // 8th hats, offbeat perc. Light swing for groove.
    pattern: [
      row('x...x.x.x.......'),
      row('....x.x.....x...'),
      row('x.x.x.x.x.x.x.x.'),
      row('..x.....x.x.....'),
    ],
  },
  {
    id: 'reggaeton',
    name: 'Reggaeton',
    bpm: 95,
    swing: 0,
    // Dembow: kick quarter-notes, snare on the "and" of 1, 2, 3 (+ backbeat).
    pattern: [
      row('x...x...x...x...'),
      row('......x...x...x.'),
      row('x.x.x.x.x.x.x.x.'),
      row('................'),
    ],
  },
  {
    id: 'disco',
    name: 'Disco',
    bpm: 120,
    swing: 0,
    // Disco: four-on-floor kick + backbeat snare + continuous 8th
    // hats + upbeat "open hat" (here approximated on the perc track).
    pattern: [
      row('x...x...x...x...'),
      row('....x.......x...'),
      row('x.x.x.x.x.x.x.x.'),
      row('..x...x...x...x.'),
    ],
  },
  {
    id: 'techno',
    name: 'Techno',
    bpm: 130,
    swing: 0,
    // Techno: steady kick + offbeat 16th hats + snare on 2 and 4 +
    // syncopated perc. Minimal and driving.
    pattern: [
      row('x...x...x...x...'),
      row('....x.......x...'),
      row('..x...x...x...x.'),
      row('....x...x...x...'),
    ],
  },

  // ---- 2 drum-and-bass presets (straight, no swing) ----

  {
    id: 'dnb-two-step',
    name: 'D&B 2-Step',
    bpm: 174,
    swing: 0,
    // Classic D&B two-step: kick on 1 and 9 (half-time feel over
    // 16ths), snare on 5 and 13, straight 16th hats.
    pattern: [
      row('x.......x.......'),
      row('....x.......x...'),
      row('x.x.x.x.x.x.x.x.'),
      row('................'),
    ],
  },
  {
    id: 'dnb-amen',
    name: 'D&B Amen',
    bpm: 174,
    swing: 0,
    // Amen-inspired rolling break: syncopated kick, snare on 5 plus
    // the "ba-dum-tss" tail (11, 13, 15), full 16th hats, ghost perc.
    pattern: [
      row('x.....x.........'),
      row('....x.....x.x.x.'),
      row('x.x.x.x.x.x.x.x.'),
      row('..x.........x...'),
    ],
  },
];
