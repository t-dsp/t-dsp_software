// loopClip.ts — the canonical MIDI-loop model + codec for the note editor.
//
// PURE module (no React, no platform imports) so it is safe to import from BOTH the
// editor UI (ui/PianoRoll.tsx) AND the platform-split transports — see the sibling-free
// rule in transport.ts / arpSeq.ts. It is also SOURCE-AGNOSTIC by construction: nothing
// here records where a model came from, so a future decodeSmf() can feed the exact same
// LoopModel the editor already edits (planning/midi-editor/DESIGN.md §5.4).
//
// The device stores a loop as a flat array of 8-byte LoopEvents (lib/TDspMidiLoop/
// src/LoopEvent.h): note-ONs and note-OFFs are SEPARATE events, there is no duration
// field, and a note held across the loop seam has its OFF stored at the WRAPPED tick
// (off.tick < on.tick). decode() pairs those into note rectangles; encode() splits them
// back. Time is 24-PPQN, loop-relative, in [0, loopTicks).

// ---- event types (must match tdsp::LoopEvType, LoopEvent.h:20-30) ----
// A plain (non-const) enum: const enums are rejected under isolatedModules (Expo's base config).
export enum LoopEvType {
  NoteOff = 0,   // d1 = note, d2 = release velocity
  AllOff = 1,    // channel-wide release
  PitchBend = 2, // bend = semitones * 256
  Timbre = 3,    // d2 = value * 255 (CC#74)
  Pressure = 4,  // d2 = value * 255 (channel pressure)
  ModWheel = 5,  // d2 = value * 255 (CC#1)
  Sustain = 6,   // d2 = 0/1 (CC#64)
  Program = 7,   // d1 = program
  NoteOn = 8,    // d1 = note, d2 = velocity 1..127
}

export const PPQN = 24;                    // ticks per quarter note (the device grid)
export const MAX_EVENTS = 1024;            // LoopClip::kMaxEvents — hard cap, silent drop on device
export const EVENT_BYTES = 8;              // sizeof(LoopEvent)
export const HEADER_BYTES = 12;            // sizeof(LoopClipHdr) (DESIGN §4.4)
const MAGIC = [0x54, 0x4c, 0x43, 0x31];    // 'T','L','C','1'

// One raw device event (mirrors LoopEvent exactly; kept for the `others` passthrough).
export type LoopEvent = {
  tick: number;    // uint16, [0, loopTicks)
  type: LoopEvType;
  ch: number;      // 1..16
  d1: number;      // note / program
  d2: number;      // velocity / value*255 / sustain
  bend: number;    // int16 semitones*256 (PitchBend only)
};

// A note rectangle for the piano roll. Derived from an on/off pair; never stored on-device.
export type LoopNote = {
  id: string;      // editor-local identity (survives re-sort); NOT persisted
  tick: number;    // start, 24-PPQN, [0, loopTicks)
  dur: number;     // ticks, always > 0; may cross the seam (see `wraps`)
  note: number;    // MIDI 0..127
  ch: number;      // 1..16
  vel: number;     // note-on velocity 1..127
  relVel: number;  // note-off release velocity (preserved; not edited in v1)
  wraps: boolean;  // tick + dur > loopTicks — rings into the loop top
};

// The canonical editor currency. Source-agnostic: no field says where it came from.
export type LoopModel = {
  notes: LoopNote[];
  others: LoopEvent[];   // bend/timbre/pressure/mod/sustain/program — PRESERVED VERBATIM
  loopTicks: number;
  beatsPerBar: number;
  bars: number;
};

// Same-tick ordering rank (mirrors loopTypeRank, LoopEvent.h:34-36): releases + expression
// BEFORE attacks, so a wrapped note-off never lands after the next iteration's note-on.
export function loopTypeRank(type: LoopEvType): number {
  return type === LoopEvType.NoteOn ? 2
    : (type === LoopEvType.NoteOff || type === LoopEvType.AllOff) ? 0 : 1;
}

// The device sort key: (tick << 3) | rank (LoopEvent.h:61-63).
export function keyOf(e: LoopEvent): number {
  return (e.tick << 3) | loopTypeRank(e.type);
}

// ---------------------------------------------------------------------------
// decode: raw clip bytes -> LoopModel
// ---------------------------------------------------------------------------

export function decode(bytes: Uint8Array): LoopModel {
  if (bytes.length < HEADER_BYTES) throw new Error('loopClip: short buffer');
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let i = 0; i < 4; i++) {
    if (dv.getUint8(i) !== MAGIC[i]) throw new Error('loopClip: bad magic (wrong version/firmware?)');
  }
  const count = dv.getUint16(4, true);
  const loopTicks = dv.getUint16(6, true);
  const beatsPerBar = dv.getUint8(8);
  const bars = dv.getUint8(9);
  const need = HEADER_BYTES + count * EVENT_BYTES;
  if (bytes.length < need) throw new Error(`loopClip: truncated (${bytes.length} < ${need})`);

  const events: LoopEvent[] = [];
  for (let i = 0; i < count; i++) {
    const o = HEADER_BYTES + i * EVENT_BYTES;
    events.push({
      tick: dv.getUint16(o, true),
      type: dv.getUint8(o + 2) as LoopEvType,
      ch: dv.getUint8(o + 3),
      d1: dv.getUint8(o + 4),
      d2: dv.getUint8(o + 5),
      bend: dv.getInt16(o + 6, true),
    });
  }
  return { ...pairEvents(events, loopTicks), loopTicks, beatsPerBar, bars };
}

// Pair note-ONs with note-OFFs into rectangles (DESIGN §5.2). Events arrive in device
// order (sorted by keyOf), so a wrapped note's OFF is seen BEFORE its ON — hence two passes.
function pairEvents(events: LoopEvent[], loopTicks: number): { notes: LoopNote[]; others: LoopEvent[] } {
  const notes: LoopNote[] = [];
  const others: LoopEvent[] = [];
  const pending = new Map<number, { ev: LoopEvent; idx: number }>();   // (ch,note) -> open note-on
  const orphanOffs: { ev: LoopEvent; idx: number }[] = [];             // offs whose on is later (wrapped)
  const key = (ch: number, note: number) => ch * 128 + note;

  events.forEach((ev, idx) => {
    if (ev.type === LoopEvType.NoteOn) {
      const prev = pending.get(key(ev.ch, ev.d1));
      if (prev) notes.push(makeNote(prev.ev, prev.idx, ev, loopTicks));   // re-press: close the old one here
      pending.set(key(ev.ch, ev.d1), { ev, idx });
    } else if (ev.type === LoopEvType.NoteOff) {
      const on = pending.get(key(ev.ch, ev.d1));
      if (on) { notes.push(makeNote(on.ev, on.idx, ev, loopTicks)); pending.delete(key(ev.ch, ev.d1)); }
      else orphanOffs.push({ ev, idx });
    } else {
      others.push(ev);
    }
  });

  // pass 2: unmatched ons pair with a wrapped orphan off of the same (ch,note).
  for (const [k, on] of pending) {
    const j = orphanOffs.findIndex(o => key(o.ev.ch, o.ev.d1) === k);
    if (j >= 0) { notes.push(makeNote(on.ev, on.idx, orphanOffs[j].ev, loopTicks)); orphanOffs.splice(j, 1); pending.delete(k); }
  }
  // pass 3 (defensive): the recorder always emits a matching off (MidiLooper.h:315-321), so
  // these fire only on a truncated transfer or a future firmware. Never render a negative rect.
  for (const [, on] of pending) {
    notes.push(makeNote(on.ev, on.idx, { ...on.ev, type: LoopEvType.NoteOff, tick: (on.ev.tick + loopTicks - 1) % loopTicks, d2: 0 }, loopTicks));
  }
  // leftover orphan offs (an off with no on anywhere): drop.

  notes.sort((a, b) => a.tick - b.tick || a.note - b.note);
  return { notes, others };
}

function makeNote(on: LoopEvent, onIdx: number, off: LoopEvent, loopTicks: number): LoopNote {
  let dur = off.tick - on.tick;
  let wraps = false;
  if (dur <= 0) { dur = off.tick + loopTicks - on.tick; wraps = true; }   // seam-crossing / re-press
  if (dur <= 0) dur = loopTicks;                                          // degenerate: full loop
  return {
    id: `n${onIdx}`, tick: on.tick, dur, note: on.d1, ch: on.ch,
    vel: on.d2, relVel: off.d2, wraps: wraps || on.tick + dur > loopTicks,
  };
}

// ---------------------------------------------------------------------------
// encode: LoopModel -> raw clip bytes (the inverse; DESIGN §5.2)
// ---------------------------------------------------------------------------

export function encode(model: LoopModel): Uint8Array {
  const events = toEvents(model);
  if (events.length > MAX_EVENTS) {
    throw new Error(`loopClip: ${events.length} events exceeds the ${MAX_EVENTS} cap`);
  }
  const buf = new ArrayBuffer(HEADER_BYTES + events.length * EVENT_BYTES);
  const dv = new DataView(buf);
  for (let i = 0; i < 4; i++) dv.setUint8(i, MAGIC[i]);
  dv.setUint16(4, events.length, true);
  dv.setUint16(6, model.loopTicks, true);
  dv.setUint8(8, model.beatsPerBar);
  dv.setUint8(9, model.bars);
  dv.setUint16(10, 0, true);   // reserved
  events.forEach((e, i) => {
    const o = HEADER_BYTES + i * EVENT_BYTES;
    dv.setUint16(o, e.tick, true);
    dv.setUint8(o + 2, e.type);
    dv.setUint8(o + 3, e.ch);
    dv.setUint8(o + 4, e.d1);
    dv.setUint8(o + 5, e.d2);
    dv.setInt16(o + 6, e.bend, true);
  });
  return new Uint8Array(buf);
}

// Split notes back into on/off events, merge the untouched `others`, and sort into the
// device's canonical (tick, rank) order. Ties broken deterministically so encode is stable.
export function toEvents(model: LoopModel): LoopEvent[] {
  const events: LoopEvent[] = [];
  for (const n of model.notes) {
    events.push({ tick: n.tick, type: LoopEvType.NoteOn, ch: n.ch, d1: n.note, d2: n.vel, bend: 0 });
    const offTick = ((n.tick + n.dur) % model.loopTicks + model.loopTicks) % model.loopTicks;
    events.push({ tick: offTick, type: LoopEvType.NoteOff, ch: n.ch, d1: n.note, d2: n.relVel, bend: 0 });
  }
  for (const e of model.others) events.push({ ...e });
  events.sort((a, b) =>
    keyOf(a) - keyOf(b) || a.ch - b.ch || a.d1 - b.d1 || a.type - b.type || a.d2 - b.d2 || a.bend - b.bend);
  return events;
}

// ---------------------------------------------------------------------------
// quantize (DESIGN §5.3). Grid values are in ticks; min duration is 1 tick, always.
// ---------------------------------------------------------------------------

export const GRIDS: Record<string, number> = {
  '1/4': 24, '1/8': 12, '1/8T': 8, '1/16': 6, '1/16T': 4, '1/32': 3,
};

export function quantizeStart(n: LoopNote, grid: number, loopTicks: number): LoopNote {
  const tick = (Math.round(n.tick / grid) * grid) % loopTicks;
  return { ...n, tick, wraps: tick + n.dur > loopTicks };
}

export function quantizeDur(n: LoopNote, grid: number, loopTicks: number): LoopNote {
  const dur = Math.max(grid, Math.round(n.dur / grid) * grid);   // never 0
  return { ...n, dur, wraps: n.tick + dur > loopTicks };
}
