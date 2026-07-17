// arpLibrary.ts — the browsable arp preset library (238 presets, 15 genres).
//
// Wraps the vendored src/arpLibrary.data.json (generated from the firmware's canonical
// lib/TDspArp/presets.json by scripts/sync-arp-library.mjs — run `npm run sync:arp` after
// the firmware library changes). Exposes typed categories + presets and an applyArpPreset()
// that pushes a preset to the device over the transport.
//
// A preset is a PARAMETER bundle (pattern + rate + gate + swing + octaves + latch, and for
// the `sequence` genre a step table). Only the fields the mix-kit firmware has an @ARP
// command for are sent; advanced fields (scale/velMode/stepMask) have no command yet and
// are ignored — most of the audible character still comes through.

import raw from './arpLibrary.data.json';
import type { Transport } from './transport';
import type { SeqStep, ArpWireParams } from './arpSeq';
import { rateIndexFromFw } from './arpSeq';

// One step as stored in the JSON library ({d,o,v}); mapped to SeqStep on apply.
type RawStep = { d: number; o: number; v: number };

// Every parameter a preset carries. All are applied to the device via one @ARPPRESET line.
export type ArpPresetParams = {
  pattern: number;      // firmware enum Pattern index (0..25)
  rate: number;         // firmware enum Rate index (0..14)
  gate: number;         // 0.05..1.5
  swing: number;        // 0.5..0.85
  octaveRange: number;  // 1..4
  octaveMode: number;
  latch: boolean;
  hold: boolean;
  velMode: number;
  velFixed: number;
  velAccent: number;
  stepMask: number;
  stepLength: number;
  mpeMode: number;
  outputChannel: number;
  scatterBase: number;
  scatterCount: number;
  scale: number;
  scaleRoot: number;
  transpose: number;
  repeat: number;
  steps?: RawStep[];    // present only on `sequence` presets
};

export type ArpPreset = {
  id: string;
  name: string;
  category: string;
  description: string;
  params: ArpPresetParams;
};

export type ArpCategory = { key: string; label: string; color: string };

// Category metadata, in the JSON's declared order (Object insertion order for string keys).
const catMap = raw.categories as Record<string, { color: string; label: string }>;
export const ARP_CATEGORIES: ArpCategory[] =
  Object.keys(catMap).map(key => ({ key, label: catMap[key].label, color: catMap[key].color }));

export const ARP_LIBRARY: ArpPreset[] = raw.presets as ArpPreset[];

export const presetsByCategory = (key: string): ArpPreset[] =>
  ARP_LIBRARY.filter(p => p.category === key);

export const presetCount = (key: string): number =>
  ARP_LIBRARY.reduce((n, p) => n + (p.category === key ? 1 : 0), 0);

const toSeqSteps = (steps: RawStep[]): SeqStep[] =>
  steps.map(s => ({ degree: s.d | 0, octave: s.o | 0, velocity: s.v | 0 }));

// What applyArpPreset returns so the caller can reflect the preset in its arp UI state.
export type AppliedArp = { pat: number; rate: number; oct: number; latch: boolean; seq?: SeqStep[] };

// Push a preset to the device and return the derived UI state. The step table (if any) is
// sent BEFORE the param bundle so it is loaded when PatUserSequence (25) engages. Every
// param rides ONE @ARPPRESET line (atomic + throttle-friendly vs. ~20 separate commands).
export function applyArpPreset(tp: Transport, p: ArpPreset, slot: 1 | 2 = 1): AppliedArp {
  const pr = p.params;
  const seq = pr.steps && pr.steps.length ? toSeqSteps(pr.steps) : undefined;
  // Call as a METHOD (not a detached ref) so `this` stays bound inside the transport.
  if (seq) { if (slot === 2) tp.arp2Sequence(seq); else tp.arpSequence(seq); }

  const wire: ArpWireParams = {
    pat: pr.pattern, rate: pr.rate,
    gatePct: Math.round((pr.gate ?? 0.5) * 100),
    swingPct: Math.round((pr.swing ?? 0.5) * 100),
    oct: pr.octaveRange, octMode: pr.octaveMode, latch: !!pr.latch,
    velMode: pr.velMode, velFixed: pr.velFixed, velAccent: pr.velAccent,
    stepMask: pr.stepMask, stepLength: pr.stepLength, mpeMode: pr.mpeMode,
    outCh: pr.outputChannel, scatterBase: pr.scatterBase, scatterCount: pr.scatterCount,
    scale: pr.scale, scaleRoot: pr.scaleRoot, transpose: pr.transpose, repeat: pr.repeat,
  };
  if (slot === 2) tp.arp2Preset(wire); else tp.arpPreset(wire);

  const oct = Math.max(1, Math.min(4, (pr.octaveRange ?? 1) | 0));
  return { pat: pr.pattern, rate: rateIndexFromFw(pr.rate), oct, latch: !!pr.latch, seq };
}
