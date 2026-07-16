// ArpStepGrid.tsx — the touch step-sequencer for the arpeggiator's User Sequence pattern.
//
// A CORE performance component: it edits a SeqStep[] (see ../arpSeq) and reports every
// change up via onChange so the parent can push it to the device (@ARPSEQ) and keep the
// arp on PatUserSequence. It is fully controlled — it holds no sequence state of its own,
// only the transient "which field a tap edits" mode.
//
// Interaction (touch-first, no long-press / no right-click needed):
//   • Shape row   — tap a built-in shape to load it wholesale.
//   • Steps −/＋   — grow/shrink the sequence length (new steps default to the root note).
//   • Edit toggle — choose whether a cell tap edits its Degree or its Octave.
//   • Step cell   — tap to cycle the active field. Degree cycles Rest → 1..8 → Chord → Rest;
//                   Octave cycles 0 → +1 → +2 → −1 → 0. A cell shows its degree big, its
//                   octave as a small badge, and an accent dot when its velocity is loud.
//
// Degrees are 1-based in the UI (musician-facing) but 0-based in the data (index into the
// held notes). Rest = "·", Chord = "C".

import React, { useState } from 'react';
import { View, Text, Pressable, ScrollView, StyleSheet } from 'react-native';
import {
  SeqStep, ArpShape, SHAPES, SEQ_REST, SEQ_CHORD, MAX_DEGREE, MAX_SEQ_STEPS,
} from '../arpSeq';

// Palette mirrors App.tsx's `C` so the grid reads as one surface with the rest of the app.
const K = {
  card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e',
  accent: '#3fb950', sel: 'rgba(31,111,235,0.28)', chip: '#21262d', rest: '#161b22',
  accentDot: '#f0a02c',
};

type EditMode = 'degree' | 'octave';

// Cycle a step's degree on tap: Rest → 0 → 1 → … → MAX_DEGREE → Chord → Rest.
function cycleDegree(d: number): number {
  if (d === SEQ_REST) return 0;
  if (d >= 0 && d < MAX_DEGREE) return d + 1;
  if (d === MAX_DEGREE) return SEQ_CHORD;
  return SEQ_REST;   // from Chord (or anything unexpected) back to Rest
}

// Cycle a step's octave on tap: 0 → +1 → +2 → −1 → 0.
function cycleOctave(o: number): number {
  if (o === 0) return 1;
  if (o === 1) return 2;
  if (o === 2) return -1;
  return 0;
}

const degreeLabel = (d: number): string =>
  d === SEQ_REST ? '·' : d === SEQ_CHORD ? 'C' : String(d + 1);   // 1-based for musicians

export type ArpStepGridProps = {
  steps: SeqStep[];
  onChange: (steps: SeqStep[]) => void;   // fires on every edit (shape load, length, cell tap)
};

export default function ArpStepGrid({ steps, onChange }: ArpStepGridProps) {
  const [mode, setMode] = useState<EditMode>('degree');

  const loadShape = (sh: ArpShape) =>
    onChange(sh.steps.map(s => ({ ...s })));   // copy so the shared SHAPES table is never mutated

  const setLength = (len: number) => {
    const target = Math.max(1, Math.min(MAX_SEQ_STEPS, len));
    if (target === steps.length) return;
    const next = steps.slice(0, target);
    while (next.length < target) next.push({ degree: 0, octave: 0, velocity: 0 });   // new steps = root note
    onChange(next);
  };

  const tapCell = (i: number) => {
    const next = steps.map((s, j) => {
      if (j !== i) return s;
      return mode === 'degree'
        ? { ...s, degree: cycleDegree(s.degree) }
        : { ...s, octave: cycleOctave(s.octave) };
    });
    onChange(next);
  };

  return (
    <View style={g.wrap}>
      {/* Shape presets — load a prebuilt arpeggiator pattern, then tweak below. */}
      <Text style={g.lbl}>Shape</Text>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} contentContainerStyle={g.shapeRow}>
        {SHAPES.map(sh => (
          <Pressable key={sh.name} style={g.shapePill} onPress={() => loadShape(sh)}>
            <Text style={g.shapeTxt}>{sh.name}</Text>
          </Pressable>
        ))}
      </ScrollView>

      {/* Length + edit-mode controls. */}
      <View style={g.ctrlRow}>
        <Text style={g.lbl}>Steps</Text>
        <Pressable style={g.stepBtn} onPress={() => setLength(steps.length - 1)}><Text style={g.stepBtnTxt}>−</Text></Pressable>
        <Text style={g.count}>{steps.length}</Text>
        <Pressable style={g.stepBtn} onPress={() => setLength(steps.length + 1)}><Text style={g.stepBtnTxt}>＋</Text></Pressable>
        <View style={{ flex: 1 }} />
        <Text style={g.lbl}>Edit</Text>
        <Pressable style={[g.modeBtn, mode === 'degree' && g.modeOn]} onPress={() => setMode('degree')}><Text style={g.modeTxt}>Note</Text></Pressable>
        <Pressable style={[g.modeBtn, mode === 'octave' && g.modeOn]} onPress={() => setMode('octave')}><Text style={g.modeTxt}>Oct</Text></Pressable>
      </View>

      {/* The step grid — wraps to as many rows as needed. */}
      <View style={g.grid}>
        {steps.map((st, i) => {
          const rest = st.degree === SEQ_REST;
          const beat = i % 4 === 0;   // subtle downbeat emphasis every 4 steps
          return (
            <Pressable key={i} onPress={() => tapCell(i)}
              style={[g.cell, beat && g.cellBeat, rest && g.cellRest, mode === 'octave' && g.cellOctMode]}>
              <Text style={[g.cellDeg, rest && g.cellDegRest]}>{degreeLabel(st.degree)}</Text>
              {st.octave !== 0 && <Text style={g.cellOct}>{st.octave > 0 ? '+' + st.octave : st.octave}</Text>}
              {st.velocity >= 100 && <View style={g.accentDot} />}
              <Text style={g.cellIdx}>{i + 1}</Text>
            </Pressable>
          );
        })}
      </View>

      <Text style={g.hint}>
        Hold a chord on the keyboard — the sequence plays it in key. Tap a step to cycle its {mode === 'degree' ? 'note (· = rest, C = full chord)' : 'octave'}.
      </Text>
    </View>
  );
}

const CELL = 44;
const g = StyleSheet.create({
  wrap: { gap: 8, marginTop: 4 },
  lbl: { color: K.muted, fontSize: 13, fontWeight: '600' },
  shapeRow: { gap: 6, paddingVertical: 2, paddingRight: 8 },
  shapePill: { backgroundColor: K.chip, paddingHorizontal: 12, paddingVertical: 7, borderRadius: 14 },
  shapeTxt: { color: K.text, fontSize: 13, fontWeight: '600' },
  ctrlRow: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 2 },
  stepBtn: { width: 34, height: 34, borderRadius: 8, borderWidth: 1, borderColor: K.border, alignItems: 'center', justifyContent: 'center' },
  stepBtnTxt: { color: K.text, fontSize: 18, fontWeight: '700', lineHeight: 20 },
  count: { color: K.text, fontSize: 15, fontWeight: '700', minWidth: 24, textAlign: 'center' },
  modeBtn: { backgroundColor: K.chip, paddingHorizontal: 12, paddingVertical: 7, borderRadius: 8 },
  modeOn: { backgroundColor: K.sel, borderWidth: 1, borderColor: K.accent },
  modeTxt: { color: K.text, fontSize: 13, fontWeight: '600' },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 4 },
  cell: { width: CELL, height: CELL, borderRadius: 8, backgroundColor: K.card2, borderWidth: 1, borderColor: K.border, alignItems: 'center', justifyContent: 'center' },
  cellBeat: { borderColor: '#495564' },
  cellRest: { backgroundColor: K.rest, borderStyle: 'dashed' },
  cellOctMode: { borderColor: K.accent },
  cellDeg: { color: K.text, fontSize: 18, fontWeight: '800' },
  cellDegRest: { color: K.muted },
  cellOct: { position: 'absolute', top: 2, right: 4, color: K.accent, fontSize: 10, fontWeight: '700' },
  cellIdx: { position: 'absolute', bottom: 1, left: 4, color: K.muted, fontSize: 9 },
  accentDot: { position: 'absolute', top: 4, left: 4, width: 6, height: 6, borderRadius: 3, backgroundColor: K.accentDot },
  hint: { color: K.muted, fontSize: 12, marginTop: 4, lineHeight: 16 },
});
