// PlaitsPanel.tsx — the Mutable Instruments Plaits VOICE EDITOR for a 'plaits' synth track.
//
// This is a TIMBRE editor, not a MIDI controller: the played note sets pitch; these controls sculpt
// the engine's tone. It mirrors the hardware faceplate's ergonomics — the model LED matrix + the
// HARMONICS / TIMBRE / MORPH macros + the LPG (decay/colour) — minus the hardware-only controls that
// have no meaning for a MIDI-driven software voice (the FREQUENCY dial, range switch, CV attenuverters).
//
// Fully controlled: it holds transient UI state (drawer open, which knob is dragging) but the model
// index + macro values are owned by App.tsx (hydrated from @STATE, echoed by @TRK). It reports changes
// up via onSelectModel (→ @TRK<i>.INSTR=) and onMacro (→ @TRK<i>.HARM=/TIMBRE=/MORPH=/LPGDECAY=/LPGCOLOR=,
// value 0..1000). onMacro is throttled here to the transport's send budget (leading+trailing, ~60 ms).
//
// Knobs are dependency-free (no react-native-svg): an LED collar of tick marks lit to the value, with a
// rotating pointer cap. Model banks colour by index — green 0..7 (pitched), red 8..15 (noise/perc/speech),
// amber 16..23 (the future Plaits 1.2 / DX7 set; lights up automatically once ninstr reports 24).

import React, { useEffect, useRef, useState } from 'react';
import { View, Text, Pressable, PanResponder, StyleSheet } from 'react-native';

// Palette mirrors App.tsx's `C` so the panel reads as one surface with the rest of the app.
const P = {
  bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d',
  text: '#e6edf3', muted: '#8b949e', faint: '#484f58', screen: '#0a1512',
};
const BANK = ['#3fb950', '#f85149', '#e3b341'];               // green / red / amber
const bankOf = (i: number) => (i < 8 ? 0 : i < 16 ? 1 : 2);
const bankName = ['Classic synthesis', 'Noise · percussion · speech', 'DX7 · physical modelling'];

// Per-model HARMONICS / TIMBRE / MORPH roles (the readout hint line), indexed to the library's engine
// order (0..15 today; 16..23 reserved for the 1.2 port). Names themselves come live from the device.
const HINTS: [string, string, string][] = [
  ['detune of 2nd osc', 'waveform (saw→square)', 'pulse width / sync'],       // 0  virtual analog
  ['fold vs shape', 'primary waveshaper', 'secondary variation'],             // 1  waveshaping
  ['frequency ratio', 'modulation index', 'operator feedback'],               // 2  two-op FM
  ['carrier/formant ratio', 'formant frequency', 'formant width'],            // 3  granular formant
  ['active harmonics', 'harmonic content', 'organ ↔ bell'],                   // 4  harmonic osc
  ['wavetable bank', 'row scan (X)', 'column scan (Y)'],                      // 5  wavetable
  ['chord type / voicing', 'inversion & register', 'voice waveform'],         // 6  chords
  ['phoneme / word bank', 'species & formant', 'word segment'],               // 7  speech
  ['grain density', 'grain duration', 'pitch randomisation'],                 // 8  granular cloud
  ['filter mode', 'cutoff', 'resonance / clocking'],                          // 9  filtered noise
  ['particle density', 'band-pass freq', 'resonance'],                        // 10 particle noise
  ['inharmonicity', 'brightness / exciter', 'decay'],                         // 11 inharmonic string
  ['material', 'brightness', 'decay'],                                        // 12 modal resonator
  ['attack / FM', 'tone & brightness', 'decay'],                             // 13 analog bass drum
  ['tone ↔ noise', 'brightness', 'decay / snappy'],                          // 14 analog snare drum
  ['tone ↔ noise', 'brightness', 'decay'],                                    // 15 analog hi-hat
];
const hintFor = (i: number): [string, string, string] => HINTS[i] ?? ['harmonics', 'timbre', 'morph'];
const stripPrefix = (s: string) => (s.includes(': ') ? s.slice(s.indexOf(': ') + 2) : s);

// ---- throttle: leading + trailing send at ~60 ms, exact final on release --------------------------
function makeThrottle(send: (v: number) => void, ms = 60) {
  let last = 0, timer: any = null, pend: number | null = null;
  const flush = () => {
    if (timer) { clearTimeout(timer); timer = null; }
    if (pend !== null) { last = Date.now(); const v = pend; pend = null; send(v); }
  };
  return {
    push(v: number) { pend = v; const dt = Date.now() - last; if (dt >= ms) flush(); else if (!timer) timer = setTimeout(flush, ms - dt); },
    flush,
  };
}

// =================================================================================================
// Knob — an LED-collar rotary. Controlled: `value` 0..1 in, onChange (optimistic, every move) +
// onCommit (send) out. Vertical drag: 160 px = full sweep. Ghost collar (optional) previews the LPG.
// =================================================================================================
const SWEEP0 = -135, SWEEP1 = 135, DRAG_PX = 160, TICKS = 25;
const angleFor = (v: number) => SWEEP0 + (SWEEP1 - SWEEP0) * v;

function Knob({ label, value, color, onChange, onCommit, size = 88, ghost }:
  { label: string; value: number; color: string; onChange: (v: number) => void; onCommit: (v: number) => void; size?: number; ghost?: number }) {
  const [dragging, setDragging] = useState(false);
  // Refs so the PanResponder (created once) always sees the latest value + callbacks (no stale closure).
  const valRef = useRef(value); valRef.current = value;
  const cb = useRef({ onChange, onCommit }); cb.current = { onChange, onCommit };
  const startV = useRef(value);
  const pan = useRef(PanResponder.create({
    onStartShouldSetPanResponder: () => true,
    onMoveShouldSetPanResponder: () => true,
    onPanResponderGrant: () => { startV.current = valRef.current; setDragging(true); },
    onPanResponderMove: (_e, g) => {
      const v = Math.max(0, Math.min(1, startV.current - g.dy / DRAG_PX));
      cb.current.onChange(v); cb.current.onCommit(v);
    },
    onPanResponderRelease: (_e, g) => {
      const v = Math.max(0, Math.min(1, startV.current - g.dy / DRAG_PX));
      setDragging(false); cb.current.onChange(v); cb.current.onCommit(v);
    },
    onPanResponderTerminate: () => setDragging(false),
  })).current;

  const R = size * 0.42, cx = size / 2, capD = size * 0.56;
  const collar = (radius: number, upTo: number, on: string, faint: string, tick = 8) =>
    Array.from({ length: TICKS }, (_, k) => {
      const frac = k / (TICKS - 1);
      const ang = SWEEP0 + (SWEEP1 - SWEEP0) * frac;
      const rad = (ang * Math.PI) / 180;
      const lit = frac <= upTo + 1e-3;
      return (
        <View key={k} pointerEvents="none" style={{
          position: 'absolute', width: 3, height: tick, borderRadius: 2,
          left: cx + radius * Math.sin(rad) - 1.5, top: cx - radius * Math.cos(rad) - tick / 2,
          backgroundColor: lit ? on : faint, transform: [{ rotate: `${ang}deg` }],
        }} />
      );
    });

  return (
    <View style={k.knobCell}>
      <View {...pan.panHandlers} style={{ width: size, height: size }}>
        {/* ghost collar (inner): a translucent preview of the LPG-opened value on Timbre/Morph */}
        {ghost != null && collar(R * 0.72, ghost, color + '66', 'transparent', 6)}
        {/* value collar (outer) */}
        {collar(R, value, color, '#1c222b')}
        {/* rotating cap + pointer */}
        <View style={{
          position: 'absolute', left: cx - capD / 2, top: cx - capD / 2, width: capD, height: capD, borderRadius: capD / 2,
          backgroundColor: '#12161c', borderWidth: 1, borderColor: dragging ? color : '#000',
          transform: [{ rotate: `${angleFor(value)}deg` }],
        }}>
          <View style={{ position: 'absolute', top: 5, left: capD / 2 - 1.5, width: 3, height: capD * 0.30, borderRadius: 2, backgroundColor: dragging ? color : P.text }} />
        </View>
      </View>
      <Text style={k.knobLabel} numberOfLines={1}>{label}</Text>
      <Text style={[k.knobVal, dragging && { color, fontWeight: '700' }]}>{Math.round(value * 100)}%</Text>
    </View>
  );
}

// =================================================================================================
// PlaitsPanel
// =================================================================================================
export type PlaitsMacros = { harm: number; timbre: number; morph: number; decay: number; color: number };
type MacroField = 'HARM' | 'TIMBRE' | 'MORPH' | 'LPGDECAY' | 'LPGCOLOR';
const DEFAULTS: PlaitsMacros = { harm: 500, timbre: 500, morph: 500, decay: 600, color: 500 };

export default function PlaitsPanel({ models, ninstr, curModel, macros, onSelectModel, onMacro }:
  { models: string[]; ninstr: number; curModel: number; macros: PlaitsMacros | undefined;
    onSelectModel: (idx: number) => void; onMacro: (field: MacroField, permille: number) => void }) {
  const n = Math.max(ninstr || models.length || 16, models.length);
  const bank = bankOf(curModel), color = BANK[bank];
  const [advanced, setAdvanced] = useState(false);

  // Optimistic macro values (0..1 floats) so knobs track the finger; re-synced from props (@STATE /
  // @TRK echoes) except while a knob is mid-drag, so a device reply can't yank the knob under the finger.
  const [vals, setVals] = useState<PlaitsMacros>(macros ?? DEFAULTS);
  const dragging = useRef(false);
  useEffect(() => { if (!dragging.current && macros) setVals(macros); }, [macros]);

  // One throttle per field, bound to the latest onMacro via a ref (send is created once).
  const onMacroRef = useRef(onMacro); onMacroRef.current = onMacro;
  const throttles = useRef<Record<MacroField, ReturnType<typeof makeThrottle>> | null>(null);
  if (!throttles.current) {
    const mk = (f: MacroField) => makeThrottle((v: number) => onMacroRef.current(f, v));
    throttles.current = { HARM: mk('HARM'), TIMBRE: mk('TIMBRE'), MORPH: mk('MORPH'), LPGDECAY: mk('LPGDECAY'), LPGCOLOR: mk('LPGCOLOR') };
  }
  const handlers = (field: MacroField, key: keyof PlaitsMacros) => ({
    onChange: (v: number) => { dragging.current = true; setVals(m => ({ ...m, [key]: Math.round(v * 1000) })); },
    onCommit: (v: number) => { throttles.current![field].push(Math.round(v * 1000)); setTimeout(() => { dragging.current = false; }, 120); },
  });

  const name = stripPrefix(models[curModel] ?? '');
  const hint = hintFor(curModel);
  const cols = 4, rows = Math.ceil(n / cols);

  return (
    <View style={k.panel}>
      {/* ---- model matrix + readout ---- */}
      <View style={k.modelRow}>
        <View style={k.matrix}>
          {Array.from({ length: rows }, (_, r) => (
            <View key={r} style={k.matrixRow}>
              {Array.from({ length: cols }, (_, c) => {
                const idx = r * cols + c;
                if (idx >= n) return <View key={c} style={k.led} />;   // spacer keeps the grid square
                const on = idx === curModel, lc = BANK[bankOf(idx)];
                return (
                  <Pressable key={c} onPress={() => onSelectModel(idx)} style={[k.led, k.ledOn && null]}
                    accessibilityLabel={`Model ${idx + 1}${on ? ', selected' : ''}`}>
                    <View style={[k.ledDot, { backgroundColor: on ? lc : lc + '22', borderColor: on ? '#fff6' : lc + '55' },
                      on && { shadowColor: lc, shadowOpacity: 0.9, shadowRadius: 6, elevation: 4 }]} />
                    <Text style={[k.ledNum, on && { color: P.text }]}>{idx + 1}</Text>
                  </Pressable>
                );
              })}
            </View>
          ))}
          <View style={k.navRow}>
            <Pressable style={k.nav} onPress={() => onSelectModel((curModel - 1 + n) % n)}><Text style={k.navTxt}>‹ Prev</Text></Pressable>
            <Pressable style={k.nav} onPress={() => onSelectModel((curModel + 1) % n)}><Text style={k.navTxt}>Next ›</Text></Pressable>
          </View>
        </View>
        <View style={[k.screen, { borderColor: color + '44' }]}>
          <View style={k.bankTag}>
            <View style={[k.chip, { backgroundColor: color }]} />
            <Text style={k.bankTxt} numberOfLines={1}>BANK {String.fromCharCode(65 + bank)} · {bankName[bank]}</Text>
          </View>
          <Text style={[k.modelName, { color }]} numberOfLines={2}>{name || '—'}</Text>
          <Text style={k.hint} numberOfLines={3}>
            <Text style={k.hintB}>HARM</Text> {hint[0]}   <Text style={k.hintB}>TIMBRE</Text> {hint[1]}   <Text style={k.hintB}>MORPH</Text> {hint[2]}
          </Text>
          <Text style={k.modelIdx}>{curModel + 1} / {n}</Text>
        </View>
      </View>

      {/* ---- macro trio ---- */}
      <View style={k.knobRow}>
        <Knob label="Harmonics" color={color} value={vals.harm / 1000} {...handlers('HARM', 'harm')} />
        <Knob label="Timbre" color={color} value={vals.timbre / 1000} ghost={Math.min(1, vals.timbre / 1000 + (vals.decay / 1000) * 0.3)} {...handlers('TIMBRE', 'timbre')} />
        <Knob label="Morph" color={color} value={vals.morph / 1000} ghost={Math.min(1, vals.morph / 1000 + (vals.decay / 1000) * 0.3)} {...handlers('MORPH', 'morph')} />
      </View>

      {/* ---- advanced drawer: LPG ---- */}
      <Pressable style={k.drawerHead} onPress={() => setAdvanced(a => !a)}>
        <Text style={k.drawerTitle}>ADVANCED · LOW-PASS GATE</Text>
        <Text style={k.caret}>{advanced ? '▾' : '▸'}</Text>
      </Pressable>
      {advanced && (
        <View style={k.knobRow}>
          <Knob label="LPG Decay" color={color} value={vals.decay / 1000} size={76} {...handlers('LPGDECAY', 'decay')} />
          <Knob label="LPG Colour" color={color} value={vals.color / 1000} size={76} {...handlers('LPGCOLOR', 'color')} />
        </View>
      )}
    </View>
  );
}

const k = StyleSheet.create({
  panel: { gap: 16, paddingVertical: 6 },
  modelRow: { flexDirection: 'row', gap: 14, flexWrap: 'wrap' },
  matrix: { gap: 8 },
  matrixRow: { flexDirection: 'row', gap: 10 },
  led: { width: 34, alignItems: 'center', gap: 3 },
  ledOn: {},
  ledDot: { width: 18, height: 18, borderRadius: 9, borderWidth: 1 },
  ledNum: { fontSize: 9, color: P.faint, fontVariant: ['tabular-nums'] },
  navRow: { flexDirection: 'row', gap: 8, marginTop: 4 },
  nav: { flex: 1, paddingVertical: 8, borderRadius: 7, borderWidth: 1, borderColor: P.border, backgroundColor: P.card2, alignItems: 'center' },
  navTxt: { color: P.text, fontSize: 12, fontWeight: '600' },
  screen: { flex: 1, minWidth: 190, borderRadius: 10, borderWidth: 1, padding: 12, backgroundColor: P.screen, gap: 4, justifyContent: 'center' },
  bankTag: { flexDirection: 'row', alignItems: 'center', gap: 7 },
  chip: { width: 9, height: 9, borderRadius: 2 },
  bankTxt: { fontSize: 10, letterSpacing: 1, color: P.muted, flexShrink: 1 },
  modelName: { fontSize: 19, fontWeight: '700' },
  hint: { fontSize: 11, color: '#5f8f78', lineHeight: 17 },
  hintB: { color: '#8fd0b0', fontWeight: '700' },
  modelIdx: { fontSize: 10, color: P.faint, fontVariant: ['tabular-nums'], marginTop: 2 },
  knobRow: { flexDirection: 'row', gap: 10, justifyContent: 'space-around', flexWrap: 'wrap', paddingVertical: 4 },
  knobCell: { alignItems: 'center', gap: 7 },
  knobLabel: { fontSize: 11, letterSpacing: 1, textTransform: 'uppercase', color: P.muted, fontWeight: '600' },
  knobVal: { fontSize: 12, color: P.text, fontVariant: ['tabular-nums'] },
  drawerHead: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingVertical: 8, borderTopWidth: 1, borderTopColor: P.border },
  drawerTitle: { fontSize: 11, letterSpacing: 1.5, color: P.muted, fontWeight: '600' },
  caret: { color: P.faint, fontSize: 12 },
});
