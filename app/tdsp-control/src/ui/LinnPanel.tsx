// Settings › LinnStrument — detect the LinnStrument on the box's USB-host port and edit every one of
// its settings from the phone. The box (lib/TDspLinn) speaks the device's NRPN dialect; this panel is
// a view over the shadow table it keeps (`linn.values`, filled by @LINN? / @LINN.V= pushes) and sends
// `@LINN.SET=<n>,<v>` for every change. The device re-reads each written value, so what you see is
// what it accepted. Layout mirrors the hardware panel: LEFT / RIGHT split tabs + a Global tab.
import React, { useMemo, useState } from 'react';
import { View, Text, Pressable, Switch, ActivityIndicator } from 'react-native';
import type { Transport } from '../transport';
import { C } from './theme';
import { s } from './styles';
import { Row, BodyTabs } from './primitives';
import {
  LinnParam, SPLIT_PARAMS, GLOBAL_PARAMS, PER_NOTE_CH_BASE, NOTE_LIGHTS_MAIN, NOTE_LIGHTS_ACCENT, NOTE_NAMES, LINN_COLOURS,
  splitId, groupsOf, noteName,
} from '../linnParams';

export interface LinnState {
  connected: boolean; isLinn: boolean; resp: number; name: string; vid: number; pid: number; cols: number;   // resp: -1 unknown, 0 not answering, 1 answering
  follow: boolean; tempo: boolean; synced: [number, number];
  values: Record<number, number>;
}
export const EMPTY_LINN: LinnState = { connected: false, isLinn: false, resp: -1, name: '', vid: 0, pid: 0, cols: 25, follow: false, tempo: false, synced: [0, 0], values: {} };

const chip = (on: boolean, disabled?: boolean) => ({
  paddingHorizontal: 10, paddingVertical: 6, borderRadius: 8, borderWidth: 1,
  backgroundColor: on ? 'rgba(63,185,80,0.18)' : C.chip, borderColor: on ? C.accent : C.border, opacity: disabled ? 0.45 : 1,
});
const Chip = ({ label, on, onPress, disabled }: { label: string; on: boolean; onPress: () => void; disabled?: boolean }) => (
  <Pressable onPress={onPress} disabled={disabled} style={chip(on, disabled)}><Text style={[s.text, { fontSize: 13 }]}>{label}</Text></Pressable>
);
const SmallBtn = ({ label, onPress, disabled }: { label: string; onPress: () => void; disabled?: boolean }) => (
  <Pressable onPress={onPress} disabled={disabled} style={[s.menuBtn, { opacity: disabled ? 0.45 : 1 }]}><Text style={s.text}>{label}</Text></Pressable>
);
const Label = ({ p }: { p: LinnParam }) => (
  <View style={{ flex: 1, minWidth: 120 }}>
    <Text style={s.text}>{p.name}</Text>
    {!!p.hint && <Text style={s.muted}>{p.hint}</Text>}
  </View>
);

export default function LinnPanel({ tp, connected, linn }: { tp: Transport; connected: boolean; linn: LinnState }) {
  const live = connected && linn.connected && linn.isLinn && linn.resp !== 0;
  const syncing = linn.synced[1] > 0 && linn.synced[0] < linn.synced[1];
  const v = (id: number): number | undefined => { const x = linn.values[id]; return typeof x === 'number' && x >= 0 ? x : undefined; };
  const set = (id: number, val: number) => tp.linn('.SET=' + id + ',' + val);

  // ---- one control per parameter kind ----
  const Control = ({ p, side }: { p: LinnParam; side: 0 | 1 }) => {
    const id = splitId(p, side);
    const cur = v(id);
    const dis = !live || syncing;
    if (p.kind === 'bool') return (
      <Row><Label p={p} /><Switch value={cur === 1} disabled={dis || cur === undefined} onValueChange={on => set(id, on ? 1 : 0)} /></Row>
    );
    if (p.kind === 'enum') return (
      <View style={{ marginVertical: 4 }}>
        <Label p={p} />
        <View style={[s.row, { marginTop: 4 }]}>
          {p.opts!.map(o => <Chip key={o.v} label={o.label} on={cur === o.v} disabled={dis} onPress={() => set(id, o.v)} />)}
          {cur !== undefined && !p.opts!.some(o => o.v === cur) && <Text style={s.muted}>(device: {cur})</Text>}
        </View>
      </View>
    );
    if (p.kind === 'trigger') return (
      <Row><Label p={p} /><SmallBtn label="Send" disabled={dis} onPress={() => set(id, 1)} /></Row>
    );
    // int / note: stepper (±1, and ±10 for wide ranges)
    const min = p.min ?? 0, max = p.max ?? 127, wide = max - min > 40;
    const shown = cur === undefined ? '—' : p.kind === 'note' ? noteName(cur) : String(cur + (p.offset ?? 0));
    const step = (d: number) => { if (cur === undefined) return; set(id, Math.max(min, Math.min(max, cur + d))); };
    return (
      <Row><Label p={p} />
        {wide && <SmallBtn label="−10" disabled={dis || cur === undefined} onPress={() => step(-10)} />}
        <SmallBtn label="−" disabled={dis || cur === undefined} onPress={() => step(-1)} />
        <Text style={[s.text, { minWidth: 44, textAlign: 'center', fontVariant: ['tabular-nums'] }]}>{shown}</Text>
        <SmallBtn label="+" disabled={dis || cur === undefined} onPress={() => step(1)} />
        {wide && <SmallBtn label="+10" disabled={dis || cur === undefined} onPress={() => step(10)} />}
      </Row>
    );
  };

  const Group = ({ title, children }: { title: string; children: React.ReactNode }) => (
    <View style={{ marginTop: 10, padding: 10, borderRadius: 10, borderWidth: 1, borderColor: C.border, backgroundColor: C.card2 }}>
      <Text style={[s.muted, { marginBottom: 4, textTransform: 'uppercase', fontSize: 11, letterSpacing: 0.6 }]}>{title}</Text>
      {children}
    </View>
  );

  // 16 per-note channel toggles (NRPN 2..17 (+100)) — only meaningful in "Channel per note"
  const PerNoteChannels = ({ side }: { side: 0 | 1 }) => (
    <View style={{ marginTop: 6 }}>
      <Text style={s.text}>Per-note channels</Text>
      <View style={[s.row, { marginTop: 4 }]}>
        {Array.from({ length: 16 }, (_, i) => {
          const id = PER_NOTE_CH_BASE + i + side * 100; const on = v(id) === 1;
          return <Chip key={i} label={String(i + 1)} on={on} disabled={!live || syncing || v(id) === undefined} onPress={() => set(id, on ? 0 : 1)} />;
        })}
      </View>
    </View>
  );
  const NoteLights = ({ base, title }: { base: number; title: string }) => (
    <View style={{ marginTop: 6 }}>
      <Text style={s.text}>{title}</Text>
      <View style={[s.row, { marginTop: 4 }]}>
        {NOTE_NAMES.map((n, i) => { const id = base + i; const on = v(id) === 1;
          return <Chip key={n} label={n} on={on} disabled={!live || syncing || v(id) === undefined} onPress={() => set(id, on ? 0 : 1)} />; })}
      </View>
    </View>
  );

  const splitBody = (side: 0 | 1) => (
    <View>
      {groupsOf(SPLIT_PARAMS).map(g => (
        <Group key={g} title={g}>
          {SPLIT_PARAMS.filter(p => p.group === g).map(p => <Control key={p.id} p={p} side={side} />)}
          {g === 'MIDI' && <PerNoteChannels side={side} />}
        </Group>
      ))}
    </View>
  );
  const globalBody = (
    <View>
      {groupsOf(GLOBAL_PARAMS).map(g => (
        <Group key={g} title={g}>
          {GLOBAL_PARAMS.filter(p => p.group === g).map(p => <Control key={p.id} p={p} side={0} />)}
          {g === 'Note lights' && <><NoteLights base={NOTE_LIGHTS_MAIN} title="Main notes lit" /><NoteLights base={NOTE_LIGHTS_ACCENT} title="Accent notes lit" /></>}
        </Group>
      ))}
      <Group title="Settings presets">
        <Text style={s.muted}>Load one of the LinnStrument's six all-settings memories (saved on the device's Preset screen).</Text>
        <View style={[s.row, { marginTop: 6 }]}>
          {[0, 1, 2, 3, 4, 5].map(i => <SmallBtn key={i} label={'Preset ' + (i + 1)} disabled={!live || syncing} onPress={() => tp.linn('.PRESET=' + i)} />)}
        </View>
      </Group>
      <Group title="Pad lights">
        <Text style={s.muted}>Paint every play pad one colour (Default restores the Note Lights display).</Text>
        <View style={[s.row, { marginTop: 6 }]}>
          {LINN_COLOURS.map((c, i) => <Chip key={c} label={c} on={false} disabled={!live} onPress={() => tp.linn('.PAINT=' + i)} />)}
          <SmallBtn label="Clear custom" disabled={!live} onPress={() => tp.linn('.LIGHTS=clear')} />
        </View>
      </Group>
    </View>
  );

  const tabs = useMemo(() => [
    { key: 'L', label: 'Left split', body: splitBody(0) },
    { key: 'R', label: 'Right split', body: splitBody(1) },
    { key: 'G', label: 'Global', body: globalBody },
  ], [linn, live, syncing]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ---- status header ----
  const dot = !connected ? C.muted : live ? C.accent : linn.connected ? '#d29922' : '#b62324';
  const headline = !connected ? 'Connect to the T-DSP first'
    : linn.connected && linn.isLinn && linn.resp === 0 ? 'LinnStrument connected but not answering'
    : live ? (linn.resp === 1 ? 'LinnStrument connected (USB host)' : 'LinnStrument connected, checking…')
    : linn.connected ? 'USB MIDI device connected, not a LinnStrument'
    : 'No LinnStrument on the USB-host port';
  const [showIds, setShowIds] = useState(false);
  return (
    <View>
      <View style={{ padding: 12, borderRadius: 10, borderWidth: 1, borderColor: C.border, backgroundColor: C.card2 }}>
        <Row>
          <View style={{ width: 14, height: 14, borderRadius: 7, backgroundColor: dot }} />
          <View style={{ flex: 1 }}>
            <Text style={s.text}>{headline}</Text>
            {linn.connected && <Text style={s.muted}>{linn.name}{showIds ? '  ·  VID ' + linn.vid.toString(16) + ' PID ' + linn.pid.toString(16) : ''}</Text>}
            {linn.connected && linn.isLinn && linn.resp === 0 && <Text style={s.muted}>It isn't accepting USB MIDI: on the device, Global Settings › Power/MIDI must be USB (not MIDI jacks), and it must be awake. Unplug and replug after changing it.</Text>}
            {!linn.connected && connected && <Text style={s.muted}>Plug it into the box's USB-host jack. Its Power/MIDI setting must be USB (the default). A LinnStrument on the DIN input plays but can't be controlled from here.</Text>}
          </View>
          {live && (syncing
            ? <View style={{ alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={s.muted}>{linn.synced[0]}/{linn.synced[1]}</Text></View>
            : <SmallBtn label="Sync" onPress={() => tp.linn('.SYNC')} />)}
        </Row>
        {linn.connected && <Pressable onPress={() => setShowIds(x => !x)}><Text style={[s.muted, { fontSize: 11, marginTop: 4 }]}>{showIds ? 'hide ids' : 'show usb ids'}</Text></Pressable>}
      </View>

      <Group title="Work with the T-DSP">
        <Row><View style={{ flex: 1 }}>
            <Text style={s.text}>Follow T-DSP's MIDI mode</Text>
            <Text style={s.muted}>When the box switches MPE on or off, both splits are set to match: channel per note from channel 1, the box's bend range, Z = channel pressure, Y = CC 74 (or one channel / bend 2 for GM).</Text>
          </View>
          <Switch value={linn.follow} disabled={!live} onValueChange={on => tp.linn('.FOLLOW=' + (on ? 1 : 0))} /></Row>
        <Row><View style={{ flex: 1 }}>
            <Text style={s.text}>Follow the master tempo</Text>
            <Text style={s.muted}>Pushes the box's BPM to the LinnStrument's clock (its arpeggiator and sequencer).</Text>
          </View>
          <Switch value={linn.tempo} disabled={!live} onValueChange={on => tp.linn('.TEMPO=' + (on ? 1 : 0))} /></Row>
        <Row><View style={{ flex: 1 }}><Text style={s.text}>Set the MPE dialect now</Text><Text style={s.muted}>One-shot version of the above.</Text></View>
          <SmallBtn label="MPE" disabled={!live} onPress={() => tp.linn('.MPE=1')} />
          <SmallBtn label="GM" disabled={!live} onPress={() => tp.linn('.MPE=0')} /></Row>
        <Row><View style={{ flex: 1 }}><Text style={s.text}>Model</Text><Text style={s.muted}>Play columns (split point, pad painting).</Text></View>
          <Chip label="LinnStrument (25)" on={linn.cols === 25} onPress={() => tp.linn('.COLS=25')} />
          <Chip label="128 (16)" on={linn.cols === 16} onPress={() => tp.linn('.COLS=16')} /></Row>
      </Group>

      <View style={{ marginTop: 10 }}>
        <BodyTabs tabs={tabs} />
      </View>
    </View>
  );
}
