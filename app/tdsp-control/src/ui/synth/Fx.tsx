// Fx.tsx — the Reverb (FX) feature, CARD + DETAIL PAGE in one file.
//   • reverbValue(fx) / reverbSubtitle(fx) → the card's value ("On"/"Off") + subtitle (Plate/Spring)
//   • reverbFoot(ctx)                       → the card's inline On/Off + wet level (tile controls)
//   • reverbBody(ctx)                       → the detail page. Two shapes by build: INSERT (master
//                                             Dry/Wet) or SEND (per-track send matrix + return), plus
//                                             the type-appropriate character sliders (plate | spring).
// Extracted verbatim from App.tsx. `ctx` carries the FX state + the per-track send state it needs
// (fx is typed loosely as the App holds extra fields the UI doesn't read; App passes its real
// fx/setFx straight through).
import React from 'react';
import { View, Text, Switch } from 'react-native';
import { s } from '../styles';
import { Row, VolSlider } from '../primitives';
import type { Transport } from '../../transport';

export type FxCtx = {
  fx: any; setFx: (updater: (f: any) => any) => void;
  tp: Transport; connected: boolean;
  synthCount: number;
  trkNames: Record<number, string>; trkEng: Record<number, string>;
  engName: (name: string, eng?: string) => string;
  trkSend: Record<number, number>; setTrkSend: (updater: (m: Record<number, number>) => Record<number, number>) => void;
  hasDrums: boolean;   // cat.hasDrums || drumLive
  audioloop: number;   // caps.audioloop (0 = no audio-loop send row)
};

export const reverbValue = (fx: any) => (fx.on ? 'On' : 'Off');
export const reverbSubtitle = (fx: any) => (fx.type === 'spring' ? 'Spring' : 'Plate') + (fx.route === 'send' ? ' · Send bus' : '');

// The type-appropriate character sliders (plate: space/shimmer/freeze; spring: time/treble/bass).
const fxCharacter = (ctx: FxCtx) => {
  const { fx, setFx, tp, connected } = ctx;
  return fx.type === 'spring' ? (
    <>
      <Text style={[s.text, { marginTop: 6 }]}>Character</Text>
      <VolSlider label="Time"   value={fx.time}   max={100} onChange={v => setFx(f => ({ ...f, time: v }))}   onCommit={v => tp.fx('TIME=' + v)}   disabled={!connected} />
      <VolSlider label="Treble" value={fx.treble} max={100} onChange={v => setFx(f => ({ ...f, treble: v }))} onCommit={v => tp.fx('TREBLE=' + v)} disabled={!connected} />
      <VolSlider label="Bass"   value={fx.bass}   max={100} onChange={v => setFx(f => ({ ...f, bass: v }))}   onCommit={v => tp.fx('BASS=' + v)}   disabled={!connected} />
    </>
  ) : (
    <>
      <Text style={[s.text, { marginTop: 6 }]}>Space</Text>
      <VolSlider label="Size"    value={fx.size} max={100} onChange={v => setFx(f => ({ ...f, size: v }))}    onCommit={v => tp.fx('SIZE=' + v)}    disabled={!connected} />
      <VolSlider label="Damping" value={fx.damp} max={100} onChange={v => setFx(f => ({ ...f, damp: v }))}    onCommit={v => tp.fx('DAMP=' + v)}    disabled={!connected} />
      <VolSlider label="Diffuse" value={fx.diff} max={100} onChange={v => setFx(f => ({ ...f, diff: v }))}    onCommit={v => tp.fx('DIFF=' + v)}    disabled={!connected} />
      <Text style={[s.text, { marginTop: 6 }]}>Shimmer</Text>
      <VolSlider label="Amount"  value={fx.shimmer} max={100} onChange={v => setFx(f => ({ ...f, shimmer: v }))} onCommit={v => tp.fx('SHIMMER=' + v)} disabled={!connected} />
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>Freeze</Text>
          <Text style={s.muted}>Hold the reverb tail infinitely.</Text>
        </View>
        <Switch value={fx.freeze} onValueChange={v => { setFx(f => ({ ...f, freeze: v })); tp.fx('FREEZE=' + (v ? 1 : 0)); }} /></Row>
    </>
  );
};

// One send-matrix row: a throttled slider (0..100) tapping a track into the reverb bus.
const fxSendRow = (ctx: FxCtx, key: string, label: string, value: number, onChange: (v: number) => void, onCommit: (v: number) => void) => (
  <VolSlider key={key} label={label} value={value} max={100} onChange={onChange} onCommit={onCommit} disabled={!ctx.connected || !ctx.fx.on} />
);

// The card's inline foot: quick On/Off + the wet level (Return in send mode, Dry/Wet Mix in insert).
export const reverbFoot = (ctx: FxCtx) => {
  const { fx, setFx, tp, connected } = ctx;
  return (
    <>
      <Row><Text style={[s.text, { flex: 1 }]}>{fx.on ? 'On' : 'Off'}</Text>
        <Switch value={fx.on} onValueChange={v => { setFx(f => ({ ...f, on: v })); tp.fx('ON=' + (v ? 1 : 0)); }} /></Row>
      {fx.route === 'send'
        ? <VolSlider label="Return" value={fx.return} max={100} onChange={v => setFx(f => ({ ...f, return: v }))} onCommit={v => tp.fx('RETURN=' + v)} disabled={!connected || !fx.on} />
        : <VolSlider label="Mix" value={fx.mix} max={100} onChange={v => setFx(f => ({ ...f, mix: v }))} onCommit={v => tp.fx('MIX=' + v)} disabled={!connected || !fx.on} />}
    </>
  );
};

// The detail page. SEND builds show a per-track send matrix + wet return; INSERT builds show the
// master Dry/Wet mix. Both append the type-appropriate character sliders.
export const reverbBody = (ctx: FxCtx) => {
  const { fx, setFx, tp, connected, synthCount, trkNames, trkEng, engName, trkSend, setTrkSend, hasDrums, audioloop } = ctx;
  return fx.route === 'send' ? (
    <>
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>Enable reverb</Text>
          <Text style={s.muted}>Turns the {fx.type === 'spring' ? 'spring' : 'plate'} reverb bus on. Raise a track's send to reverb it; sends at 0 stay dry.</Text>
        </View>
        <Switch value={fx.on} onValueChange={v => { setFx(f => ({ ...f, on: v })); tp.fx('ON=' + (v ? 1 : 0)); }} /></Row>
      <Text style={[s.text, { marginTop: 8 }]}>Sends</Text>
      <Text style={s.muted}>How much of each track feeds the reverb. 0 = fully dry.</Text>
      {Array.from({ length: synthCount }).map((_, v) =>
        fxSendRow(ctx, 'snd' + v, 'Synth ' + String.fromCharCode(65 + v) + (trkNames[v] ? ' · ' + engName(trkNames[v], trkEng[v]) : ''),
          trkSend[v] ?? 0, val => setTrkSend(m => ({ ...m, [v]: val })), val => tp.trk(v, 'FXSEND=' + val)))}
      {hasDrums &&
        fxSendRow(ctx, 'snddrum', 'Drums', trkSend[synthCount] ?? 0, val => setTrkSend(m => ({ ...m, [synthCount]: val })), val => tp.trk(synthCount, 'FXSEND=' + val))}
      {audioloop > 0 &&
        fxSendRow(ctx, 'sndloop', 'Audio Loop', fx.loopsend, val => setFx(f => ({ ...f, loopsend: val })), val => tp.fx('LOOPSEND=' + val))}
      <Text style={[s.text, { marginTop: 10 }]}>Return</Text>
      <VolSlider label="Wet" value={fx.return} max={100} onChange={v => setFx(f => ({ ...f, return: v }))} onCommit={v => tp.fx('RETURN=' + v)} disabled={!connected || !fx.on} />
      {fxCharacter(ctx)}
    </>
  ) : (
    <>
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>Enable reverb</Text>
          <Text style={s.muted}>Inserts the {fx.type === 'spring' ? 'spring' : 'plate'} reverb on the master output. Off = clean (bypassed).</Text>
        </View>
        <Switch value={fx.on} onValueChange={v => { setFx(f => ({ ...f, on: v })); tp.fx('ON=' + (v ? 1 : 0)); }} /></Row>
      <Text style={[s.text, { marginTop: 6 }]}>Dry / Wet</Text>
      <VolSlider label="Mix" value={fx.mix} max={100} onChange={v => setFx(f => ({ ...f, mix: v }))} onCommit={v => tp.fx('MIX=' + v)} disabled={!connected} />
      {fxCharacter(ctx)}
    </>
  );
};
