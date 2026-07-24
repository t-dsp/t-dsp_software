// BeatStrip — the header's tempo dots (one circle per beat of the bar). Split out of
// primitives.tsx into src/ui/elements/ so the header's own visual pieces live together.
//
// Header beat lights: one circle per beat of the bar, the current beat lit, beat 1 (the
// downbeat / accent) in a distinct color. Two clock sources, preferring the device:
//   • LIVE (`live`): the device's @BEAT feed — locked to the REAL master downbeat + meter.
//   • LOCAL fallback: a self-correcting clock at the master BPM, for firmware that doesn't
//     emit @BEAT. Matches the tempo but re-anchors its downbeat to "now" on tempo/meter change.
// Always visible: unlit when idle, lit dots track the beat when running.
// Only this component re-renders each beat (its own state), not the whole app.
import React, { useState, useRef, useEffect } from 'react';
import { View } from 'react-native';
import { s } from '../styles';

export function BeatStrip({ sig, bpm, active, live }: { sig: number; bpm: number; active: boolean; live: { i: number; n: number } | null }) {
  const [beat, setBeat] = useState(-1);
  const anchor = useRef(0);
  const useLive = active && !!live;
  const beats = useLive ? live!.n : (sig || 4);   // device meter when live, else the @METROSIG setting (fallback 4 so the strip is always shown)
  useEffect(() => {
    if (useLive || !active || beats < 1 || bpm <= 0) { setBeat(-1); return; }   // live feed drives it directly — no local timer
    const period = 60000 / bpm;   // ms per quarter-note beat
    const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());
    anchor.current = now();
    let timer: any;
    // Self-correcting: each tick schedules the NEXT beat boundary off the fixed anchor,
    // so setTimeout jitter never accumulates into drift.
    const tick = () => {
      const t = now();
      const i = Math.floor((t - anchor.current) / period);
      setBeat(((i % beats) + beats) % beats);
      timer = setTimeout(tick, Math.max(0, anchor.current + (i + 1) * period - t));
    };
    tick();
    return () => clearTimeout(timer);
  }, [beats, bpm, active, useLive]);
  if (beats < 1) return null;   // always visible: unlit when idle, lit dots track the beat when running
  const lit = useLive ? live!.i : beat;
  return (
    <View style={s.beatStrip}>
      {Array.from({ length: beats }, (_, i) => {
        const on = i === lit, down = i === 0;
        return <View key={i} style={[s.beatDot, down && s.beatDotDown, on && (down ? s.beatDotDownOn : s.beatDotOn)]} />;
      })}
    </View>
  );
}
