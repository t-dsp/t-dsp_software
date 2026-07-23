// PianoRoll.tsx — touch-first note editor for a recorded MIDI loop.
//
// Keyboard rail on the left, a pitch x time grid to the right; notes are rectangles you can
// move, resize (right-edge handle, appears only on the selected note) and quantize to the grid.
// Select-then-act with an explicit tool palette (Select / Add / Erase) — the touch answer to
// "no hover, no right-click" (planning/midi-editor/DESIGN.md §6.2). Edits are LOCAL until you
// press Commit, which streams the clip back to the device (§6.3).
//
// This is the composition layer: it owns the codec (loopClip) + transport wiring, and drives
// the MIDI-FREE geometry/touch helpers in pianoRollGeom. Per DESIGN §6.4 the geometry module
// never learns MIDI — the pitch<->lane mapping lives HERE, so an arp roll can reuse that module.

import React, { useEffect, useRef, useState } from 'react';
import { View, Text, Pressable, ScrollView, PanResponder, StyleSheet, useWindowDimensions } from 'react-native';
import {
  decode, encode, quantizeStart, GRIDS, MAX_EVENTS,
  LoopModel, LoopNote, LoopEvent,
} from '../loopClip';
import {
  Viewport, hitTest, applyDrag, DragState, DEAD_PX, toCol, toLane, snap, clamp, gx, gy,
} from './pianoRollGeom';
import type { Transport } from '../transport';

const K = {
  card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e',
  accent: '#f85149', sel: '#58a6ff', grid: '#1c232d', bar: '#3a4653', key: '#12181f',
  keyBlack: '#0a0e13', note: '#f85149', noteSel: '#58a6ff', chip: '#21262d',
};

const ROW_H = 20;                 // px per pitch row
const RAIL_W = 46;                // keyboard rail width
const EDGE_PX = 22;               // resize-handle / hit margin (fat-finger)
const VEL_H = 54;                 // velocity lane height
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const isBlack = (m: number) => [1, 3, 6, 8, 10].includes(((m % 12) + 12) % 12);
const noteName = (m: number) => NOTE_NAMES[((m % 12) + 12) % 12] + (Math.floor(m / 12) - 1);

const GRID_LABELS: [string, number][] = Object.entries(GRIDS);   // [['1/4',24],...]

type Tool = 'select' | 'add' | 'erase';
type Meta = { loopTicks: number; beatsPerBar: number; bars: number };

export type PianoRollProps = {
  tp: Transport;
  voice: number;   // 1-based synth voice
  maxEvents?: number;    // per-clip event cap from @STATE rec.max (default MAX_EVENTS)
  onClose?: () => void;
};

export default function PianoRoll({ tp, voice, maxEvents = MAX_EVENTS, onClose }: PianoRollProps) {
  const [loading, setLoading] = useState(true);
  const [err, setErr] = useState<string | null>(null);
  const [notes, setNotes] = useState<LoopNote[]>([]);
  const [others, setOthers] = useState<LoopEvent[]>([]);
  const [meta, setMeta] = useState<Meta>({ loopTicks: 0, beatsPerBar: 4, bars: 1 });
  const [win, setWin] = useState<{ lo: number; hi: number }>({ lo: 60, hi: 83 });
  const [tool, setTool] = useState<Tool>('select');
  const [gridTicks, setGridTicks] = useState(GRIDS['1/16']);
  const [colW, setColW] = useState(4);
  const [sel, setSel] = useState<string | null>(null);
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);

  const undo = useRef<LoopNote[][]>([]);
  const idc = useRef(1);

  // Full-BLEED: the editor lives inside a maxWidth:720 centered page. Measure our left offset
  // in the window and pull back to the screen edge so the roll uses the FULL window width.
  const { width: winW } = useWindowDimensions();
  const [offX, setOffX] = useState(0);
  const bleedRef = useRef<View>(null);
  const remeasure = () => bleedRef.current?.measureInWindow?.((x: number) => setOffX(x || 0));
  useEffect(() => { remeasure(); }, [winW, loading]);

  // ---- load the recorded clip on mount / voice change ----
  useEffect(() => {
    let live = true;
    setLoading(true); setErr(null);
    tp.recDump(voice).then(bytes => {
      if (!live) return;
      const m: LoopModel = decode(bytes);
      setNotes(m.notes); setOthers(m.others);
      setMeta({ loopTicks: m.loopTicks, beatsPerBar: m.beatsPerBar, bars: m.bars });
      setWin(pitchWindow(m.notes));
      setSel(null); setDirty(false); undo.current = [];
      setLoading(false);
    }).catch(e => { if (live) { setErr(String(e?.message ?? e)); setLoading(false); } });
    return () => { live = false; };
  }, [tp, voice]);

  const rows = win.hi - win.lo + 1;
  const vp: Viewport = { colW, rowH: ROW_H, cols: meta.loopTicks, rows };
  const eventCount = notes.length * 2 + others.length;
  const atCap = eventCount >= maxEvents;

  // Latest-state refs so the once-created PanResponder reads fresh values.
  const R = useRef({ notes, tool, gridTicks, sel, vp, win, meta, atCap });
  R.current = { notes, tool, gridTicks, sel, vp, win, meta, atCap };
  const drag = useRef<DragState>({ mode: 'none', id: null, baseStart: 0, baseLength: 0, baseLane: 0, committed: false });

  const pushUndo = (snapshot: LoopNote[]) => {
    undo.current.push(snapshot.map(n => ({ ...n })));
    if (undo.current.length > 20) undo.current.shift();
  };
  const laneOf = (note: number) => R.current.win.hi - note;
  const noteAtLane = (lane: number) => R.current.win.hi - lane;

  const pan = useRef(PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onMoveShouldSetPanResponder: () => true,
    onPanResponderGrant: (e) => {
      const { locationX: x, locationY: y } = e.nativeEvent;
      const s = R.current;
      const items = s.notes.map(n => ({ id: n.id, start: n.tick, length: n.dur, lane: laneOf(n.note) }));
      const hit = hitTest(items, x, y, s.vp, EDGE_PX);

      if (s.tool === 'erase') {
        if (hit) { pushUndo(s.notes); setNotes(prev => prev.filter(n => n.id !== hit.item.id)); setDirty(true); setSel(null); }
        drag.current.mode = 'none';
        return;
      }
      if (hit && !(s.tool === 'add' && false)) {
        // resize only when the note is ALREADY selected and the grab is on its right edge.
        const resizing = hit.onRightEdge && hit.item.id === s.sel;
        setSel(hit.item.id);
        pushUndo(s.notes);
        drag.current = {
          mode: resizing ? 'resize' : 'move', id: hit.item.id,
          baseStart: hit.item.start, baseLength: hit.item.length, baseLane: hit.item.lane, committed: false,
        };
        return;
      }
      if (s.tool === 'add' && !s.atCap) {
        const tick = clamp(snap(toCol(x, s.vp), s.gridTicks), 0, Math.max(0, s.meta.loopTicks - s.gridTicks));
        const lane = clamp(toLane(y, s.vp), 0, s.vp.rows - 1);
        const id = 'e' + (idc.current++);
        const n: LoopNote = { id, tick, dur: s.gridTicks, note: noteAtLane(lane), ch: 1, vel: 100, relVel: 0, wraps: false };
        pushUndo(s.notes);
        setNotes(prev => [...prev, n]); setSel(id); setDirty(true);
        drag.current = { mode: 'create', id, baseStart: tick, baseLength: s.gridTicks, baseLane: lane, committed: false };
        return;
      }
      // empty tap in Select -> deselect
      setSel(null);
      drag.current.mode = 'none';
    },
    onPanResponderMove: (_e, g) => {
      const d = drag.current;
      if (d.mode === 'none' || !d.id) return;
      if (!d.committed && Math.hypot(g.dx, g.dy) < DEAD_PX) return;
      d.committed = true;
      const s = R.current;
      const dCol = g.dx / s.vp.colW;
      const dLane = Math.round(g.dy / s.vp.rowH);
      const geom = applyDrag(d, dCol, dLane, s.gridTicks, s.vp);
      const id = d.id;
      setNotes(prev => prev.map(n => n.id !== id ? n : {
        ...n, tick: geom.start, dur: geom.length, note: noteAtLane(geom.lane),
        wraps: geom.start + geom.length > s.meta.loopTicks,
      }));
    },
    onPanResponderRelease: () => { finishDrag(); },
    onPanResponderTerminate: () => { finishDrag(); },
  })).current;

  const finishDrag = () => {
    const d = drag.current;
    if (d.mode !== 'none') {
      if (d.committed || d.mode === 'create') setDirty(true);
      else undo.current.pop();   // it was only a tap (select) — discard the snapshot we took
    }
    drag.current.mode = 'none';
  };

  // ---- selected-note edits (the floating context bar) ----
  const editSel = (fn: (n: LoopNote) => LoopNote) => {
    if (!sel) return;
    pushUndo(notes);
    setNotes(prev => prev.map(n => (n.id === sel ? fn(n) : n)));
    setDirty(true);
  };
  const bumpVel = (d: number) => editSel(n => ({ ...n, vel: clamp(n.vel + d, 1, 127) }));
  const dupSel = () => {
    const n = notes.find(x => x.id === sel); if (!n) return;
    const id = 'e' + (idc.current++);
    const tick = clamp(n.tick + n.dur, 0, Math.max(0, meta.loopTicks - 1));
    pushUndo(notes);
    setNotes(prev => [...prev, { ...n, id, tick, wraps: tick + n.dur > meta.loopTicks }]);
    setSel(id); setDirty(true);
  };
  const delSel = () => { if (!sel) return; pushUndo(notes); setNotes(prev => prev.filter(n => n.id !== sel)); setSel(null); setDirty(true); };

  const doQuantize = () => {
    const target = sel ? new Set([sel]) : null;
    pushUndo(notes);
    setNotes(prev => prev.map(n => (!target || target.has(n.id)) ? quantizeStart(n, gridTicks, meta.loopTicks) : n));
    setDirty(true);
  };
  const doUndo = () => { const s = undo.current.pop(); if (s) { setNotes(s); setDirty(true); setSel(null); } };

  const commit = async () => {
    setBusy(true); setErr(null);
    try {
      const bytes = encode({ notes, others, ...meta });
      await tp.recLoad(voice, bytes);
      setDirty(false);
    } catch (e: any) { setErr(String(e?.message ?? e)); }
    finally { setBusy(false); }
  };

  // ---- render ----
  if (loading) return <View style={s.center}><Text style={s.muted}>Loading loop…</Text></View>;
  if (err && !notes.length) return (
    <View style={s.center}>
      <Text style={[s.muted, { color: K.accent }]}>{err}</Text>
      <Text style={s.muted}>Record a loop first, then edit it here.</Text>
    </View>
  );
  if (!meta.loopTicks) return <View style={s.center}><Text style={s.muted}>No loop recorded on this voice yet.</Text></View>;

  const gridW = meta.loopTicks * colW;
  const beatTicks = 24, barTicks = meta.beatsPerBar * 24;
  const beats = Math.round(meta.loopTicks / beatTicks);
  const selNote = notes.find(n => n.id === sel) || null;

  return (
   <View ref={bleedRef} onLayout={remeasure} style={{ width: '100%' }}>
    <View style={[s.wrap, { width: winW, marginLeft: -offX, paddingHorizontal: 8 }]}>
      {/* toolbar */}
      <View style={s.toolbar}>
        {(['select', 'add', 'erase'] as Tool[]).map(t => (
          <Pressable key={t} onPress={() => setTool(t)} style={[s.tool, tool === t && s.toolOn]}>
            <Text style={[s.toolTxt, tool === t && s.toolTxtOn]}>{t === 'select' ? 'Select' : t === 'add' ? '＋ Add' : 'Erase'}</Text>
          </Pressable>
        ))}
        <View style={{ width: 8 }} />
        <Text style={s.muted}>Grid</Text>
        <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={{ gap: 4 }}>
          {GRID_LABELS.map(([label, t]) => (
            <Pressable key={label} onPress={() => setGridTicks(t)} style={[s.chip, gridTicks === t && s.chipOn]}>
              <Text style={[s.chipTxt, gridTicks === t && s.chipTxtOn]}>{label}</Text>
            </Pressable>
          ))}
        </ScrollView>
      </View>

      <View style={s.toolbar}>
        <Pressable onPress={doQuantize} style={s.btn}><Text style={s.btnTxt}>Quantize{sel ? ' sel' : ' all'}</Text></Pressable>
        <Pressable onPress={doUndo} style={s.btn}><Text style={s.btnTxt}>↶ Undo</Text></Pressable>
        <Pressable onPress={() => setColW(c => clamp(c - 1, 2, 12))} style={s.btnSq}><Text style={s.btnTxt}>−</Text></Pressable>
        <Pressable onPress={() => setColW(c => clamp(c + 1, 2, 12))} style={s.btnSq}><Text style={s.btnTxt}>＋</Text></Pressable>
        <View style={{ flex: 1 }} />
        <Text style={[s.muted, atCap && { color: K.accent }]}>{eventCount}/{maxEvents}</Text>
        <Pressable onPress={commit} disabled={busy || !dirty} style={[s.commit, (busy || !dirty) && s.commitOff]}>
          <Text style={s.commitTxt}>{busy ? '…' : dirty ? 'Commit' : 'Saved'}</Text>
        </Pressable>
      </View>
      {err ? <Text style={[s.muted, { color: K.accent, paddingHorizontal: 8 }]}>{err}</Text> : null}

      {/* keyboard rail + scrollable grid */}
      <ScrollView style={{ maxHeight: rows * ROW_H + VEL_H + 8 }}>
        <View style={{ flexDirection: 'row' }}>
          {/* rail */}
          <View style={{ width: RAIL_W }}>
            {Array.from({ length: rows }, (_, lane) => {
              const m = win.hi - lane;
              return (
                <View key={m} style={[s.key, { height: ROW_H, backgroundColor: isBlack(m) ? K.keyBlack : K.key }]}>
                  {m % 12 === 0 && <Text style={s.keyLabel}>{noteName(m)}</Text>}
                </View>
              );
            })}
          </View>
          {/* grid (horizontally scrollable) — flex:1 so it fills the full-bleed width */}
          <ScrollView horizontal showsHorizontalScrollIndicator style={{ flex: 1 }}>
            <View>
              <View style={{ width: gridW, height: rows * ROW_H }} {...pan.panHandlers}>
                {/* row shading */}
                {Array.from({ length: rows }, (_, lane) => (
                  <View key={'r' + lane} style={{ position: 'absolute', top: lane * ROW_H, left: 0, width: gridW, height: ROW_H, backgroundColor: isBlack(win.hi - lane) ? K.card2 : 'transparent', borderBottomWidth: 1, borderBottomColor: K.grid }} />
                ))}
                {/* beat / bar lines */}
                {Array.from({ length: beats + 1 }, (_, b) => (
                  <View key={'b' + b} style={{ position: 'absolute', left: b * beatTicks * colW, top: 0, width: (b * beatTicks) % barTicks === 0 ? 2 : 1, height: rows * ROW_H, backgroundColor: (b * beatTicks) % barTicks === 0 ? K.bar : K.grid }} />
                ))}
                {/* notes */}
                {notes.map(n => {
                  const wrapW = Math.min(n.dur, meta.loopTicks - n.tick);   // draw up to the seam; tail wraps visually below
                  const selected = n.id === sel;
                  return (
                    <React.Fragment key={n.id}>
                      <View style={{ position: 'absolute', left: n.tick * colW, top: laneOf(n.note) * ROW_H + 1, width: Math.max(3, wrapW * colW), height: ROW_H - 2, backgroundColor: selected ? K.noteSel : K.note, borderRadius: 3, opacity: 0.35 + 0.5 * (n.vel / 127) }}>
                        {selected && <View style={s.handle} />}
                      </View>
                      {n.wraps && (
                        <View style={{ position: 'absolute', left: 0, top: laneOf(n.note) * ROW_H + 1, width: Math.max(3, (n.tick + n.dur - meta.loopTicks) * colW), height: ROW_H - 2, backgroundColor: selected ? K.noteSel : K.note, borderRadius: 3, opacity: 0.25 + 0.4 * (n.vel / 127) }} />
                      )}
                    </React.Fragment>
                  );
                })}
              </View>
              {/* velocity lane */}
              <View style={{ width: gridW, height: VEL_H, borderTopWidth: 1, borderTopColor: K.border, marginTop: 2 }}>
                {notes.map(n => (
                  <View key={'v' + n.id} style={{ position: 'absolute', left: n.tick * colW, bottom: 0, width: Math.max(2, colW), height: (VEL_H - 4) * (n.vel / 127), backgroundColor: n.id === sel ? K.noteSel : K.note, opacity: 0.8 }} />
                ))}
                <Text style={s.velLabel}>vel</Text>
              </View>
            </View>
          </ScrollView>
        </View>
      </ScrollView>

      {/* floating context bar for the selected note */}
      {selNote && (
        <View style={s.ctxBar}>
          <Text style={s.ctxInfo}>{noteName(selNote.note)} · v{selNote.vel}</Text>
          <Pressable onPress={() => bumpVel(-8)} style={s.ctxBtn}><Text style={s.ctxTxt}>vel −</Text></Pressable>
          <Pressable onPress={() => bumpVel(+8)} style={s.ctxBtn}><Text style={s.ctxTxt}>vel ＋</Text></Pressable>
          <Pressable onPress={dupSel} style={s.ctxBtn}><Text style={s.ctxTxt}>Dup</Text></Pressable>
          <Pressable onPress={delSel} style={[s.ctxBtn, { backgroundColor: K.accent }]}><Text style={[s.ctxTxt, { color: '#fff' }]}>Delete</Text></Pressable>
        </View>
      )}
    </View>
   </View>
  );
}

// Fixed pitch window for the session: the span the clip uses, padded to octave boundaries and
// at least two octaves. Lanes are clamped to this, so notes never leave the visible range.
function pitchWindow(notes: LoopNote[]): { lo: number; hi: number } {
  let lo = 60, hi = 71;
  if (notes.length) { lo = Math.min(...notes.map(n => n.note)); hi = Math.max(...notes.map(n => n.note)); }
  lo = Math.floor(lo / 12) * 12;
  hi = Math.ceil((hi + 1) / 12) * 12 - 1;
  while (hi - lo < 23) { if (hi < 127) hi += 12; if (hi - lo < 23 && lo > 0) lo -= 12; if (lo === 0 && hi === 127) break; }
  return { lo: Math.max(0, lo), hi: Math.min(127, hi) };
}

const s = StyleSheet.create({
  wrap: { gap: 6, marginTop: 4 },
  center: { padding: 20, alignItems: 'center', gap: 6 },
  muted: { color: K.muted, fontSize: 13 },
  toolbar: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  tool: { backgroundColor: K.chip, paddingHorizontal: 12, paddingVertical: 7, borderRadius: 8 },
  toolOn: { backgroundColor: K.sel },
  toolTxt: { color: K.text, fontSize: 13, fontWeight: '600' },
  toolTxtOn: { color: '#fff' },
  chip: { backgroundColor: K.chip, paddingHorizontal: 10, paddingVertical: 6, borderRadius: 12 },
  chipOn: { backgroundColor: K.sel },
  chipTxt: { color: K.text, fontSize: 12, fontWeight: '600' },
  chipTxtOn: { color: '#fff' },
  btn: { backgroundColor: K.chip, paddingHorizontal: 10, paddingVertical: 7, borderRadius: 8 },
  btnSq: { backgroundColor: K.chip, width: 34, height: 32, borderRadius: 8, alignItems: 'center', justifyContent: 'center' },
  btnTxt: { color: K.text, fontSize: 13, fontWeight: '600' },
  commit: { backgroundColor: K.accent, paddingHorizontal: 14, paddingVertical: 7, borderRadius: 8 },
  commitOff: { backgroundColor: K.chip },
  commitTxt: { color: '#fff', fontSize: 13, fontWeight: '700' },
  key: { borderBottomWidth: 1, borderBottomColor: K.border, justifyContent: 'center', paddingLeft: 4 },
  keyLabel: { color: K.muted, fontSize: 9 },
  handle: { position: 'absolute', right: 0, top: 0, bottom: 0, width: 6, backgroundColor: '#fff', opacity: 0.7, borderTopRightRadius: 3, borderBottomRightRadius: 3 },
  velLabel: { position: 'absolute', left: 3, top: 2, color: K.muted, fontSize: 9 },
  ctxBar: { flexDirection: 'row', alignItems: 'center', gap: 6, backgroundColor: K.card, borderWidth: 1, borderColor: K.border, borderRadius: 10, padding: 6 },
  ctxInfo: { color: K.text, fontSize: 12, fontWeight: '700', paddingHorizontal: 4 },
  ctxBtn: { backgroundColor: K.chip, paddingHorizontal: 10, paddingVertical: 7, borderRadius: 8 },
  ctxTxt: { color: K.text, fontSize: 12, fontWeight: '600' },
});
