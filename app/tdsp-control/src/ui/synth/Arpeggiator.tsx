// Arpeggiator.tsx — the Arpeggiator feature for a synth voice, CARD + DETAIL PAGE in one file.
//   • arpValue(slot)   → the card's live value line ("Preset — none picked" / pattern·rate)
//   • arpActions(slot)  → the card's play controls (‹ › ▶ ■)
//   • arpBody(slot)     → the detail page: Play/Stop, Latch, Preset|Manual tabs, pattern grid,
//                         rate/octaves, the User-Sequence step grid, and the preset browser.
// Extracted verbatim from App.tsx; parameterized entirely by an ArpSlot (state + handlers), so it's
// engine-agnostic and App wires it by passing the per-voice slot. Signatures match App's originals
// (arpValue/arpActions/arpBody), so wiring is a drop-in import.
import React from 'react';
import { View, Text, Pressable, Switch } from 'react-native';
import { s } from '../styles';
import { HdrBtn, Row } from '../primitives';
import ArpPresetBrowser from '../ArpPresetBrowser';
import ArpStepGrid from '../ArpStepGrid';
import { ARP_PATTERNS as ARP_PAT, ARP_RATES, PAT_USER_SEQUENCE } from '../../arpSeq';
import type { ArpSlot } from './types';

export const arpValue = (A: ArpSlot) => (A.arp.on ? '' : '(off)  ') + (A.mode === 'preset'
  ? (A.activeName || 'Preset — none picked')
  : ARP_PAT[A.arp.pat] + '  ·  ' + ARP_RATES[A.arp.rate].label);

export const arpActions = (A: ArpSlot) => (<>
  {/* leading spacer right-justifies the (50px-capped) transport controls on the card */}
  <View style={{ flex: 1 }} />
  <HdrBtn label="‹" stop onPress={() => A.stepNav(-1)} cap />
  <HdrBtn label="›" stop onPress={() => A.stepNav(1)} cap />
  <HdrBtn label="▶" onPress={A.play} active={A.arp.on} cap />
  <HdrBtn label="■" stop onPress={A.stop} cap />
</>);

export const arpBody = (A: ArpSlot) => (
  <>
    {/* Play and Stop are independent (not a toggle): Play re-triggers a running arp,
        Stop bypasses it. The active state is shown by which button is highlighted. */}
    <Row>
      <Pressable style={[s.btn, s.grow1, A.arp.on ? s.btnOn : s.btnIdle]} onPress={A.play}>
        <Text style={s.btnText}>▶  {A.arp.on ? 'Restart' : 'Play'}</Text>
      </Pressable>
      <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={A.stop}>
        <Text style={s.btnText}>■  Stop</Text>
      </Pressable>
    </Row>
    {/* Latch is always visible (both Preset and Manual modes) so it's clear whether the
        arp keeps running after the keys release — and can always be turned back off. */}
    <Row><View style={{ flex: 1 }}>
        <Text style={s.text}>Latch</Text>
        <Text style={s.muted}>Keep arpeggiating after you release the keys.</Text>
      </View>
      <Switch value={A.arp.latch} onValueChange={A.setLatch} /></Row>
    <View style={s.arpTabs}>
      <Pressable style={[s.arpTab, A.mode === 'preset' && s.arpTabOn]} onPress={() => A.setMode('preset')}>
        <Text style={[s.arpTabTxt, A.mode === 'preset' && s.arpTabTxtOn]}>Presets</Text>
      </Pressable>
      <Pressable style={[s.arpTab, A.mode === 'manual' && s.arpTabOn]} onPress={A.enterManual}>
        <Text style={[s.arpTabTxt, A.mode === 'manual' && s.arpTabTxtOn]}>Manual</Text>
      </Pressable>
    </View>
    {A.mode === 'preset' ? (
      <>
        <Text style={s.muted}>{A.activeName ? 'Active preset: ' + A.activeName : 'Pick a preset — it sets everything (pattern, rate, feel, scale…). Switch to Manual to tweak.'}</Text>
        <ArpPresetBrowser onApply={A.applyPreset} activeId={A.presetId} />
      </>
    ) : (
      <>
        <Text style={s.muted}>Pattern</Text>
        <View style={s.patGrid}>
          {ARP_PAT.map((p, i) => <Pressable key={i} style={[s.patCell, A.arp.pat === i && s.patCellOn]} onPress={() => A.selectPattern(i)}><Text style={s.patCellTxt} numberOfLines={1}>{p}</Text></Pressable>)}
        </View>
        {A.arp.pat === PAT_USER_SEQUENCE && <ArpStepGrid steps={A.seq} onChange={A.applySeq} />}
        <Row><Text style={[s.muted, { flex: 1 }]}>Rate</Text>
          {ARP_RATES.map((r, i) => <Pressable key={i} style={[s.pill, A.arp.rate === i && s.pillOn]} onPress={() => A.setRate(i)}><Text style={s.text}>{r.label}</Text></Pressable>)}</Row>
        <Row><Text style={[s.muted, { flex: 1 }]}>Octaves {A.arp.oct}</Text>
          {[1, 2, 3, 4].map(n => <Pressable key={n} style={[s.pill, A.arp.oct === n && s.pillOn]} onPress={() => A.setOct(n)}><Text style={s.text}>{n}</Text></Pressable>)}</Row>
        <Pressable style={[s.btn, s.btnGhost, s.btnWide]} onPress={A.resetManual}>
          <Text style={s.btnText}>Reset to plain arp</Text>
        </Pressable>
      </>
    )}
  </>
);
