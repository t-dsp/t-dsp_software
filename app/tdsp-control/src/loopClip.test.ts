// loopClip.test.ts — the off-target correctness check the design hangs on
// (planning/midi-editor/DESIGN.md §8.1). Pure TS, no hardware. Run:
//   npx tsc src/loopClip.ts src/loopClip.test.ts --outDir <tmp> --module commonjs --target es2020
//   node --test <tmp>
// (a scratch runner script does this; see scripts/test-codec.mjs)

// This file runs under `node --test`, not Metro. scripts/test-codec.mjs compiles it and
// tolerates the node:test/node:assert "cannot find name" warnings (no @types/node in the RN
// app — it would leak Node globals into the app typecheck). The codec itself (loopClip.ts) is
// strictly typechecked by the app-wide `tsc --noEmit`; here node --test is the correctness gate.
// @ts-nocheck
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  LoopEvType, LoopEvent, LoopModel, LoopNote, keyOf, loopTypeRank,
  decode, encode, toEvents, quantizeStart, quantizeDur, HEADER_BYTES, EVENT_BYTES,
} from './loopClip';

// ---- fixture helpers -------------------------------------------------------

// Serialize a raw event list into clip bytes, in the exact device order (sorted by keyOf,
// then a deterministic tiebreak matching toEvents). Lets a test hand-craft a wire buffer.
function bytesOf(meta: { loopTicks: number; beatsPerBar: number; bars: number }, events: LoopEvent[]): Uint8Array {
  const evs = [...events].sort((a, b) =>
    keyOf(a) - keyOf(b) || a.ch - b.ch || a.d1 - b.d1 || a.type - b.type || a.d2 - b.d2 || a.bend - b.bend);
  const buf = new ArrayBuffer(HEADER_BYTES + evs.length * EVENT_BYTES);
  const dv = new DataView(buf);
  [0x54, 0x4c, 0x43, 0x31].forEach((b, i) => dv.setUint8(i, b));
  dv.setUint16(4, evs.length, true);
  dv.setUint16(6, meta.loopTicks, true);
  dv.setUint8(8, meta.beatsPerBar);
  dv.setUint8(9, meta.bars);
  dv.setUint16(10, 0, true);
  evs.forEach((e, i) => {
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

const ev = (tick: number, type: LoopEvType, ch: number, d1: number, d2: number, bend = 0): LoopEvent =>
  ({ tick, type, ch, d1, d2, bend });
const noteOn = (tick: number, note: number, vel = 100, ch = 1) => ev(tick, LoopEvType.NoteOn, ch, note, vel);
const noteOff = (tick: number, note: number, rel = 0, ch = 1) => ev(tick, LoopEvType.NoteOff, ch, note, rel);

const meta = { loopTicks: 96, beatsPerBar: 4, bars: 1 };   // 1 bar of 4/4 @ 24 PPQN

// Normalize a model so two orderings compare equal.
function norm(m: LoopModel) {
  return {
    notes: [...m.notes].map(n => ({ ...n, id: '' })).sort((a, b) => a.tick - b.tick || a.note - b.note || a.ch - b.ch),
    others: [...m.others].sort((a, b) => keyOf(a) - keyOf(b) || a.type - b.type || a.d2 - b.d2 || a.bend - b.bend),
    loopTicks: m.loopTicks, beatsPerBar: m.beatsPerBar, bars: m.bars,
  };
}

// ---- decode / pairing ------------------------------------------------------

test('plain note pairs into one rectangle', () => {
  const m = decode(bytesOf(meta, [noteOn(24, 60, 100), noteOff(48, 60, 0)]));
  assert.equal(m.notes.length, 1);
  const n = m.notes[0];
  assert.deepEqual([n.tick, n.dur, n.note, n.vel, n.wraps], [24, 24, 60, 100, false]);
});

test('a chord — multiple note-ons at one tick — pairs to N notes', () => {
  const m = decode(bytesOf(meta, [
    noteOn(0, 60), noteOn(0, 64), noteOn(0, 67),
    noteOff(24, 60), noteOff(24, 64), noteOff(24, 67),
  ]));
  assert.equal(m.notes.length, 3);
  assert.deepEqual(m.notes.map(n => n.note).sort((a, b) => a - b), [60, 64, 67]);
  assert.ok(m.notes.every(n => n.tick === 0 && n.dur === 24 && !n.wraps));
});

test('wrapped note (off.tick < on.tick) becomes one wrapping rectangle', () => {
  // note-on at 84 (near the end), released at 12 in the NEXT iteration -> off stored at wrapped tick 12.
  const m = decode(bytesOf(meta, [noteOn(84, 48), noteOff(12, 48)]));
  assert.equal(m.notes.length, 1);
  const n = m.notes[0];
  assert.equal(n.tick, 84);
  assert.equal(n.dur, 12 + 96 - 84);   // 24
  assert.equal(n.wraps, true);
});

test('re-pressed held note (on, on, off, off) yields two notes', () => {
  const m = decode(bytesOf(meta, [noteOn(0, 60), noteOn(24, 60), noteOff(48, 60), noteOff(72, 60)]));
  assert.equal(m.notes.length, 2);
});

test('note at tick 0 and at loopTicks-1 survive', () => {
  const m = decode(bytesOf(meta, [
    noteOn(0, 40), noteOff(6, 40),
    noteOn(95, 80), noteOff(3, 80),   // starts on the last tick, wraps
  ]));
  assert.equal(m.notes.length, 2);
  assert.ok(m.notes.some(n => n.tick === 0));
  assert.ok(m.notes.some(n => n.tick === 95 && n.wraps));
});

test('expression events are preserved verbatim in `others`', () => {
  const bytes = bytesOf(meta, [
    noteOn(0, 60), noteOff(24, 60),
    ev(0, LoopEvType.PitchBend, 1, 0, 0, 512),   // +2 semitones
    ev(12, LoopEvType.Pressure, 1, 0, 200),
    ev(0, LoopEvType.Program, 1, 5, 0),
  ]);
  const m = decode(bytes);
  assert.equal(m.notes.length, 1);
  assert.equal(m.others.length, 3);
  assert.ok(m.others.some(o => o.type === LoopEvType.PitchBend && o.bend === 512));
  assert.ok(m.others.some(o => o.type === LoopEvType.Pressure && o.d2 === 200));
});

// ---- round trips -----------------------------------------------------------

test('encode(decode(bytes)) round-trips the MODEL (the load->dump invariant)', () => {
  const cases: Uint8Array[] = [
    bytesOf(meta, [noteOn(24, 60, 100), noteOff(48, 60, 20)]),
    bytesOf(meta, [noteOn(0, 60), noteOn(0, 64), noteOff(24, 60), noteOff(24, 64)]),
    bytesOf(meta, [noteOn(84, 48), noteOff(12, 48)]),                       // wrapped
    bytesOf(meta, [noteOn(0, 60), ev(0, LoopEvType.PitchBend, 1, 0, 0, 300), noteOff(24, 60)]),
  ];
  for (const b of cases) {
    const once = decode(b);
    const twice = decode(encode(once));
    assert.deepEqual(norm(twice), norm(once));
  }
});

test('encode is byte-identical for a canonically-ordered clip', () => {
  const b = bytesOf(meta, [
    noteOn(0, 60, 100), noteOff(24, 60, 10),
    noteOn(24, 67, 90), noteOff(48, 67, 10),
  ]);
  assert.deepEqual(encode(decode(b)), b);
});

test('encoded event stream honors the (tick, rank) sort invariant', () => {
  const m = decode(bytesOf(meta, [noteOn(0, 60), noteOn(0, 64), noteOff(0, 55), noteOff(24, 60), noteOff(24, 64)]));
  const evs = toEvents(m);
  for (let i = 1; i < evs.length; i++) {
    assert.ok(keyOf(evs[i - 1]) <= keyOf(evs[i]), `event ${i} out of order`);
    // and at equal tick, a release/expression never lands after an attack:
    if (evs[i - 1].tick === evs[i].tick) {
      assert.ok(loopTypeRank(evs[i - 1].type) <= loopTypeRank(evs[i].type));
    }
  }
});

// ---- quantize --------------------------------------------------------------

const mkNote = (tick: number, dur: number): LoopNote =>
  ({ id: 'x', tick, dur, note: 60, ch: 1, vel: 100, relVel: 0, wraps: false });

test('quantizeStart snaps to the grid and stays in range', () => {
  assert.equal(quantizeStart(mkNote(7, 12), 6, 96).tick, 6);
  assert.equal(quantizeStart(mkNote(11, 12), 6, 96).tick, 12);
  assert.equal(quantizeStart(mkNote(95, 12), 6, 96).tick, 0);   // rounds to 96 -> wraps to 0
});

test('quantizeDur never produces a zero-length note', () => {
  assert.equal(quantizeDur(mkNote(0, 1), 6, 96).dur, 6);    // rounds toward 0 -> clamped to one grid unit
  assert.equal(quantizeDur(mkNote(0, 20), 6, 96).dur, 18);
  assert.ok(quantizeDur(mkNote(0, 2), 3, 96).dur >= 3);
});

// ---- guards ----------------------------------------------------------------

test('decode rejects a bad magic', () => {
  const b = bytesOf(meta, [noteOn(0, 60), noteOff(24, 60)]);
  b[0] = 0x58;   // corrupt 'T' -> 'X'
  assert.throws(() => decode(b), /magic/);
});

test('decode rejects a truncated buffer', () => {
  const b = bytesOf(meta, [noteOn(0, 60), noteOff(24, 60)]);
  assert.throws(() => decode(b.slice(0, HEADER_BYTES + 3)), /truncated/);
});

test('encode throws past the 1024-event cap', () => {
  const notes: LoopNote[] = [];
  for (let i = 0; i < 513; i++) notes.push(mkNote((i * 2) % 96, 1));   // 513 notes = 1026 events
  assert.throws(() => encode({ notes, others: [], ...meta }), /cap/);
});
