// MpeMonitor.tsx — Settings > MPE Monitor. A live view of the firmware's @MPEMON trace so you
// can SEE exactly what an MPE controller (LinnStrument on the Teensy USB-host port) sends and
// what actually reaches the synth — the two ends of the note/bend/pressure chain.
//
// Two taps come over the wire (see src/ui/mpeBus.ts and firmware mpeMonEmit):
//   INPUT  (dir u/d/b/s) — raw controller MIDI, before any routing/arp/gating.
//   OUTPUT (dir o)       — post router+arp, exactly what the synth sink received.
// A note-off (or a bend) that shows up at INPUT but never at OUTPUT is being eaten in between —
// the classic symptom of an arp re-channeling MPE member notes so the per-note synth can't match
// the release (stuck notes) or the bend (dead drag). The "Chain" panel calls that out directly.
//
// Perf: @MPE lines arrive in bursts (a drag ~100/s). We never touch React state per event —
// events land in refs; a rAF loop repaints only when something changed. Same isolation trick as
// the catalog ProgressBus/LoadScreen, so the high-rate feed can't starve the Web Serial reader.

import React, { useEffect, useRef, useState } from 'react';
import { View, Text, Pressable, ScrollView, Switch, StyleSheet } from 'react-native';
import type { Transport } from '../transport';
import { mpeBus, isInput, MpeEvent } from './mpeBus';

const K = {
  card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e',
  accent: '#ff7b72', good: '#3fb950', warn: '#f0883e', bad: '#f85149', in: '#3fb950', out: '#58a6ff',
  key: '#12181f', keyBlack: '#080b0f', chip: '#21262d', grid: '#1c232d',
};

// One vivid, well-separated hue per MPE member channel (2..16). Channel 1 (master) is grey.
const CH_COLORS = [
  '#8b949e', '#8b949e', '#58a6ff', '#3fb950', '#f0883e', '#a371f7', '#f778ba', '#56d4dd',
  '#e3b341', '#ff7b72', '#79c0ff', '#7ee787', '#ffa657', '#d2a8ff', '#ff9bce', '#39c5cf',
];
const chColor = (ch: number) => CH_COLORS[ch >= 1 && ch <= 16 ? ch : 0] ?? '#8b949e';

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const isBlack = (m: number) => [1, 3, 6, 8, 10].includes(((m % 12) + 12) % 12);
const noteName = (m: number) => (m < 0 || m > 127) ? '--' : NOTE_NAMES[((m % 12) + 12) % 12] + (Math.floor(m / 12) - 1);

// Chromatic pitch ruler bounds. Linear (equal px/semitone) so a bend's drag reads as literal
// horizontal distance — clearer than a real piano for showing pitch travel.
const NOTE_LO = 36, NOTE_HI = 84;   // C2..C6
const KEY_W = 15;                    // px per semitone cell
const RULER_W = (NOTE_HI - NOTE_LO + 1) * KEY_W;

// Per-channel live state (kept in a ref, mutated on every event, read at frame rate).
type ChanState = {
  noteIn: number;      // active INPUT note (-1 = none)
  velIn: number;
  bendRaw: number;     // INPUT raw pitch-bend -8192..8191
  pressIn: number;     // 0..1
  timbreIn: number;    // 0..1 (CC74)
  noteOut: number;     // active OUTPUT (synth-side) note (-1 = none)
  bendSemiOut: number; // OUTPUT bend in semitones
  pressOut: number;    // 0..1
  timbreOut: number;   // 0..1
  at: number;          // last-touched Date.now()
};
const newChan = (): ChanState => ({
  noteIn: -1, velIn: 0, bendRaw: 0, pressIn: 0, timbreIn: 0.5,
  noteOut: -1, bendSemiOut: 0, pressOut: 0, timbreOut: 0.5, at: 0,
});

const EV_LABEL: Record<string, string> = { n: 'NoteOn', x: 'NoteOff', b: 'Bend', p: 'Press', c: 'CC' };
const DIR_LABEL: Record<string, string> = { u: 'USB', d: 'DIN', b: 'BT', s: 'SER', o: 'SYNTH' };

export type MpeMonitorProps = { tp: Transport; connected: boolean };

export default function MpeMonitor({ tp, connected }: MpeMonitorProps) {
  const [on, setOn] = useState(true);            // monitor enabled (auto-on when page opens)
  const [mpe, setMpe] = useState(false);         // global MPE mode (@MIDIMODE) — board boots OFF, no @STATE round-trip
  const [range, setRange] = useState(24);        // bend range for INPUT raw->semitone display (2/24/48)
  const [, force] = useState(0);                 // visualizer repaint trigger (paced to VIZ_MS)
  const [logSnap, setLogSnap] = useState<MpeEvent[]>([]);   // raw-log rows, refreshed slower (LOG_MS) into a memoized child
  const lastViz = useRef(0);                     // last visualizer paint (ms)
  const lastLog = useRef(0);                     // last log-snapshot refresh (ms)

  const chans = useRef<ChanState[]>(Array.from({ length: 17 }, newChan));   // index by channel 1..16
  const log = useRef<MpeEvent[]>([]);                                        // newest first, capped
  const heldIn = useRef<Set<string>>(new Set());                            // "ch:note" pressed at INPUT
  const heldOut = useRef<Set<string>>(new Set());                           // "ch:note" sounding at OUTPUT
  const counts = useRef({ inOn: 0, inOff: 0, outOn: 0, outOff: 0, bendIn: 0, bendOut: 0 });
  const dirty = useRef(false);
  const rangeRef = useRef(range); rangeRef.current = range;

  // Enable/disable the device trace to match the toggle + connection.
  useEffect(() => {
    if (connected && on) tp.mpeMonitor(true);
    return () => { try { tp.mpeMonitor(false); } catch {} };
  }, [connected, on, tp]);

  // Ingest events into refs (no React state — cheap under burst).
  useEffect(() => mpeBus.subscribe((e: MpeEvent) => {
    // Raw log (bounded ring, newest first).
    const lg = log.current; lg.unshift(e); if (lg.length > 300) lg.length = 300;

    const c = chans.current[e.ch] || (chans.current[e.ch] = newChan());
    c.at = e.at;
    const key = e.ch + ':' + e.v1;
    if (isInput(e.dir)) {
      if (e.ev === 'n') { heldIn.current.add(key); counts.current.inOn++; c.noteIn = e.v1; c.velIn = e.v2; c.bendRaw = 0; }
      else if (e.ev === 'x') { heldIn.current.delete(key); counts.current.inOff++; if (c.noteIn === e.v1) c.noteIn = -1; }
      else if (e.ev === 'b') { c.bendRaw = e.v1; counts.current.bendIn++; }
      else if (e.ev === 'p') { c.pressIn = e.v1 / 127; }
      else if (e.ev === 'c') { if (e.v1 === 74) c.timbreIn = e.v2 / 127; }
    } else if (e.dir === 'o') {
      if (e.ev === 'n') { heldOut.current.add(key); counts.current.outOn++; c.noteOut = e.v1; c.bendSemiOut = 0; }
      else if (e.ev === 'x') {
        if (e.v1 === 255) { // all-notes-off for this channel
          for (const k of Array.from(heldOut.current)) if (k.startsWith(e.ch + ':')) heldOut.current.delete(k);
          c.noteOut = -1;
        } else { heldOut.current.delete(key); counts.current.outOff++; if (c.noteOut === e.v1) c.noteOut = -1; }
      }
      else if (e.ev === 'b') { c.bendSemiOut = e.v1 / 100; counts.current.bendOut++; }
      else if (e.ev === 'p') { c.pressOut = e.v1 / 1000; }
      else if (e.ev === 'c') { if (e.v1 === 74) c.timbreOut = e.v2 / 1000; }
    }
    dirty.current = true;
  }), []);

  // Paint on a fixed clock, DECOUPLED from the ~100/s event rate, so a burst can't force
  // 60 full re-renders/sec and starve the serial reader (which showed up as the note lagging
  // far behind the finger). The cheap visualizer repaints at VIZ_MS reading the latest refs;
  // the heavy raw log refreshes at LOG_MS into a memoized child (so it isn't rebuilt per viz
  // frame). Both only fire when new data actually arrived (dirty), so idle costs nothing.
  useEffect(() => {
    const VIZ_MS = 33, LOG_MS = 140;   // ~30 fps visualizer, ~7 fps log
    let raf = 0;
    const loop = () => {
      if (dirty.current) {
        const now = Date.now();
        if (now - lastViz.current >= VIZ_MS) { lastViz.current = now; dirty.current = false; force(x => (x + 1) & 0xffff); }
        if (now - lastLog.current >= LOG_MS) {
          lastLog.current = now;
          const next = log.current.slice(0, 40);
          setLogSnap(prev => (prev.length === next.length && prev[0] === next[0]) ? prev : next);   // keep ref stable when unchanged -> memo skips
        }
      }
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, []);

  const clear = () => {
    log.current = []; heldIn.current.clear(); heldOut.current.clear();
    counts.current = { inOn: 0, inOff: 0, outOn: 0, outOff: 0, bendIn: 0, bendOut: 0 };
    chans.current = Array.from({ length: 17 }, newChan);
    setLogSnap([]);
    force(x => (x + 1) & 0xffff);
  };

  // Derived view data.
  const cs = chans.current;
  const activeChans: number[] = [];
  for (let ch = 1; ch <= 16; ch++) if (cs[ch] && (cs[ch].noteIn >= 0 || cs[ch].noteOut >= 0)) activeChans.push(ch);
  const stuckOut = heldOut.current.size;
  const stuck = heldIn.current.size === 0 && stuckOut > 0;   // input released everything but synth still holds
  const c = counts.current;

  const bendSemisIn = (ch: number) => (cs[ch].bendRaw >= 0 ? cs[ch].bendRaw / 8191 : cs[ch].bendRaw / 8192) * rangeRef.current;

  return (
    <View style={{ gap: 12 }}>
      {/* ---- global MPE mode: the actual expression switch (a full slide only bends ~2 semis when OFF) ---- */}
      <View style={[st.panel, { flexDirection: 'row', alignItems: 'center', gap: 12, borderColor: mpe ? K.good : K.warn }]}>
        <Switch value={mpe} onValueChange={v => { setMpe(v); tp.midiMode(v); }} disabled={!connected} />
        <View style={{ flex: 1 }}>
          <Text style={st.label}>MPE mode {mpe ? '· ON — per-note bend ±24' : '· OFF — normal MIDI, bend ±2'}</Text>
          <Text style={st.muted}>
            {mpe
              ? 'Per-note pitch/pressure/Y for a LinnStrument; ch10 is a melodic member.'
              : 'A full LinnStrument slide only bends ~2 semitones until this is ON. (Board boots OFF.)'}
          </Text>
        </View>
      </View>

      {/* ---- controls ---- */}
      <View style={st.rowWrap}>
        <View style={st.ctlRow}>
          <Text style={st.label}>Monitor</Text>
          <Switch value={on} onValueChange={setOn} />
          <View style={[st.dot, { backgroundColor: on && connected ? K.good : K.muted }]} />
          <Text style={st.muted}>{!connected ? 'not connected' : on ? 'live' : 'paused'}</Text>
        </View>
        <View style={st.ctlRow}>
          <Text style={st.label}>Bend ±</Text>
          {[2, 24, 48].map(r => (
            <Pressable key={r} onPress={() => setRange(r)} style={[st.pill, range === r && st.pillOn]}>
              <Text style={[st.pillT, range === r && st.pillTOn]}>{r}</Text>
            </Pressable>
          ))}
          <Pressable onPress={clear} style={st.pill}><Text style={st.pillT}>Clear</Text></Pressable>
        </View>
      </View>
      <Text style={st.muted}>
        Play the LinnStrument: this shows every command it sends (green = from controller) and what the
        synth actually receives (blue = SYNTH, post-arp). Bend ± only scales the INPUT drag readout.
      </Text>

      {/* ---- chain health ---- */}
      <View style={[st.panel, stuck && { borderColor: K.bad }]}>
        <Text style={st.section}>Chain</Text>
        <View style={st.rowWrap}>
          <Stat label="IN note on/off" v={`${c.inOn} / ${c.inOff}`} />
          <Stat label="SYNTH on/off" v={`${c.outOn} / ${c.outOff}`} />
          <Stat label="held: IN" v={String(heldIn.current.size)} color={heldIn.current.size ? K.in : K.muted} />
          <Stat label="held: SYNTH" v={String(stuckOut)} color={stuck ? K.bad : stuckOut ? K.out : K.muted} />
          <Stat label="bend IN/SYNTH" v={`${c.bendIn} / ${c.bendOut}`} />
        </View>
        {stuck && (
          <Text style={[st.muted, { color: K.bad }]}>
            ⚠ {stuckOut} note{stuckOut > 1 ? 's' : ''} still sounding at the synth after the controller
            released everything — the release is being lost between INPUT and SYNTH (arp re-channeling?).
          </Text>
        )}
        {!stuck && c.bendIn > 4 && c.bendOut === 0 && (
          <Text style={[st.muted, { color: K.warn }]}>
            ⚠ Controller sent {c.bendIn} pitch-bends but the synth received 0 — the drag isn't reaching
            the note (arp re-channeling the bend, or a per-note synth with no engine on that channel).
          </Text>
        )}
      </View>

      {/* ---- pitch ruler visualizer: notes + drags + pressure ---- */}
      <View style={st.panel}>
        <Text style={st.section}>Notes · Drags · Pressure  <Text style={st.muted}>(INPUT)</Text></Text>
        <ScrollView horizontal showsHorizontalScrollIndicator style={{ marginTop: 6 }}>
          <View style={{ width: RULER_W, height: 132 }}>
            {/* static semitone cells — memoized so they aren't rebuilt on every viz frame */}
            <RulerCells />
            {/* active notes: pressure bar on home key + drag line to bent pitch */}
            {activeChans.filter(ch => cs[ch].noteIn >= 0).map(ch => {
              const st0 = cs[ch];
              const homeX = (st0.noteIn - NOTE_LO) * KEY_W + KEY_W / 2;
              const bentX = (st0.noteIn - NOTE_LO + bendSemisIn(ch)) * KEY_W + KEY_W / 2;
              const col = chColor(ch);
              const zH = 8 + st0.pressIn * 96;        // pressure -> bar height
              return (
                <React.Fragment key={ch}>
                  {/* pressure column on the home key */}
                  <View style={{ position: 'absolute', left: homeX - 5, bottom: 16, width: 10, height: zH, backgroundColor: col, opacity: 0.85, borderRadius: 2 }} />
                  {/* drag line from home -> bent pitch */}
                  <View style={{ position: 'absolute', left: Math.min(homeX, bentX), top: 20, width: Math.max(2, Math.abs(bentX - homeX)), height: 3, backgroundColor: col, opacity: 0.9 }} />
                  {/* bent-pitch head */}
                  <View style={{ position: 'absolute', left: bentX - 6, top: 14, width: 12, height: 12, borderRadius: 6, backgroundColor: col, borderWidth: 2, borderColor: '#0d1117' }} />
                  {/* label */}
                  <Text style={{ position: 'absolute', left: homeX - 14, top: 2, width: 40, fontSize: 9, color: col, textAlign: 'center' }}>
                    {noteName(st0.noteIn)}·{ch}
                  </Text>
                  {/* timbre (Y) tick under the key */}
                  <View style={{ position: 'absolute', left: homeX - 7 + st0.timbreIn * 14 - 7, bottom: 4, width: 4, height: 8, backgroundColor: '#e3b341', opacity: 0.9, borderRadius: 1 }} />
                </React.Fragment>
              );
            })}
          </View>
        </ScrollView>
        {activeChans.filter(ch => cs[ch].noteIn >= 0).length === 0 && (
          <Text style={[st.muted, { marginTop: 4 }]}>No notes held — touch the controller.</Text>
        )}
      </View>

      {/* ---- per-channel numeric table ---- */}
      {activeChans.length > 0 && (
        <View style={st.panel}>
          <Text style={st.section}>Per-channel</Text>
          <View style={[st.trow, { borderBottomWidth: 1, borderColor: K.border, paddingBottom: 4 }]}>
            <Text style={[st.th, { width: 34 }]}>ch</Text>
            <Text style={[st.th, { width: 92 }]}>note (IN / SYNTH)</Text>
            <Text style={[st.th, { width: 118 }]}>bend semis (IN / SYNTH)</Text>
            <Text style={[st.th, { width: 92 }]}>Z (IN / SYNTH)</Text>
            <Text style={[st.th, { width: 92 }]}>Y (IN / SYNTH)</Text>
          </View>
          {activeChans.map(ch => {
            const s0 = cs[ch]; const col = chColor(ch);
            return (
              <View key={ch} style={st.trow}>
                <View style={[st.dot, { backgroundColor: col, marginRight: 6 }]} />
                <Text style={[st.td, { width: 24 }]}>{ch}</Text>
                <Text style={[st.td, { width: 92 }]}>{s0.noteIn >= 0 ? noteName(s0.noteIn) : '·'} / {s0.noteOut >= 0 ? noteName(s0.noteOut) : '·'}</Text>
                <Text style={[st.td, { width: 118 }]}>{bendSemisIn(ch).toFixed(2)} / {s0.bendSemiOut.toFixed(2)}</Text>
                <Text style={[st.td, { width: 92 }]}>{(s0.pressIn * 100).toFixed(0)}% / {(s0.pressOut * 100).toFixed(0)}%</Text>
                <Text style={[st.td, { width: 92 }]}>{(s0.timbreIn * 100).toFixed(0)}% / {(s0.timbreOut * 100).toFixed(0)}%</Text>
              </View>
            );
          })}
        </View>
      )}

      {/* ---- raw command log (memoized child, refreshed at LOG_MS not per viz frame) ---- */}
      <View style={st.panel}>
        <Text style={st.section}>Serial commands  <Text style={st.muted}>(newest first)</Text></Text>
        <RawLog rows={logSnap} />
      </View>
    </View>
  );
}

// The static pitch ruler (semitone cells + octave labels). Never changes, so memo() renders it
// once and skips it on every visualizer repaint — only the live note overlays repaint.
const RulerCells = React.memo(function RulerCells() {
  return (
    <>
      {Array.from({ length: NOTE_HI - NOTE_LO + 1 }, (_, i) => {
        const m = NOTE_LO + i;
        return (
          <View key={m} style={[st.cell, { left: i * KEY_W, width: KEY_W, backgroundColor: isBlack(m) ? K.keyBlack : K.key }]}>
            {m % 12 === 0 && <Text style={st.octLabel}>C{Math.floor(m / 12) - 1}</Text>}
          </View>
        );
      })}
    </>
  );
});

// The raw command list. Heaviest part of the page, so it lives in its own memo() and only
// re-renders when its `rows` reference changes (refreshed ~7 fps), not with the 30 fps viz.
const RawLog = React.memo(function RawLog({ rows }: { rows: MpeEvent[] }) {
  if (rows.length === 0) return <Text style={[st.muted, { marginTop: 4 }]}>Waiting for @MPE trace… play the controller.</Text>;
  return (
    <View style={{ marginTop: 4 }}>
      {rows.map((e, i) => {
        const input = isInput(e.dir);
        const col = e.dir === 'o' ? K.out : K.in;
        return (
          <Text key={i} style={[st.logLine, { color: input ? K.text : K.muted }]} numberOfLines={1}>
            <Text style={{ color: col }}>{input ? '▸' : '◂'} {DIR_LABEL[e.dir] || e.dir}</Text>
            {'  '}{(EV_LABEL[e.ev] || e.ev).padEnd(7, ' ')} ch{String(e.ch).padStart(2, ' ')}  {fmtVals(e)}
          </Text>
        );
      })}
    </View>
  );
});

// Human-readable value column per event (raw for INPUT, normalized for SYNTH/OUTPUT).
function fmtVals(e: MpeEvent): string {
  const out = e.dir === 'o';
  switch (e.ev) {
    case 'n': return `${noteName(e.v1)} v${e.v2}`;
    case 'x': return e.v1 === 255 ? 'ALL' : `${noteName(e.v1)}`;
    case 'b': return out ? `${(e.v1 / 100).toFixed(2)} st` : `raw ${e.v1}`;
    case 'p': return out ? `${(e.v1 / 1000 * 100).toFixed(0)}%` : `${e.v1}`;
    case 'c': return out ? `cc${e.v1}=${(e.v2 / 1000 * 100).toFixed(0)}%` : `cc${e.v1}=${e.v2}`;
    default: return `${e.v1} ${e.v2}`;
  }
}

function Stat({ label, v, color }: { label: string; v: string; color?: string }) {
  return (
    <View style={st.stat}>
      <Text style={[st.statV, color ? { color } : null]}>{v}</Text>
      <Text style={st.statL}>{label}</Text>
    </View>
  );
}

const st = StyleSheet.create({
  rowWrap: { flexDirection: 'row', flexWrap: 'wrap', gap: 14, alignItems: 'center' },
  ctlRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  label: { color: K.text, fontSize: 13, fontWeight: '600' },
  muted: { color: K.muted, fontSize: 12, lineHeight: 17 },
  dot: { width: 10, height: 10, borderRadius: 5 },
  pill: { paddingHorizontal: 10, paddingVertical: 5, borderRadius: 7, backgroundColor: K.chip, borderWidth: 1, borderColor: K.border },
  pillOn: { backgroundColor: K.accent + '33', borderColor: K.accent },
  pillT: { color: K.muted, fontSize: 12, fontWeight: '600' },
  pillTOn: { color: K.text },
  panel: { backgroundColor: K.card2, borderWidth: 1, borderColor: K.border, borderRadius: 10, padding: 10, gap: 4 },
  section: { color: K.text, fontSize: 13, fontWeight: '700' },
  stat: { minWidth: 96 },
  statV: { color: K.text, fontSize: 17, fontWeight: '700', fontVariant: ['tabular-nums'] },
  statL: { color: K.muted, fontSize: 11 },
  cell: { position: 'absolute', top: 0, height: 132, borderRightWidth: 1, borderColor: '#00000055' },
  octLabel: { position: 'absolute', bottom: 2, left: 2, fontSize: 8, color: K.muted },
  trow: { flexDirection: 'row', alignItems: 'center', paddingVertical: 3 },
  th: { color: K.muted, fontSize: 11, fontWeight: '600' },
  td: { color: K.text, fontSize: 12, fontVariant: ['tabular-nums'] },
  logLine: { fontFamily: Platform_OS_mono(), fontSize: 11, lineHeight: 16 },
});

// Monospace font hint that works on web + native without importing Platform at module top twice.
function Platform_OS_mono(): any {
  // react-native-web maps 'monospace' fine; native falls back to its default mono.
  return 'monospace';
}
