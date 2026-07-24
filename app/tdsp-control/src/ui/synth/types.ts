// types.ts — shared prop shapes for the extracted synth-section components (src/ui/synth/*).
// These mirror the interfaces App.tsx builds internally (ArpSlotT, SongDeckT); they're duplicated
// here — structurally identical — so each component is a clean drop-in when App wires it up (an
// App value typed ArpSlotT is assignable to ArpSlot by structural typing, no cast needed).
import type { SeqStep } from '../../arpSeq';
import type { ArpPreset as LibArpPreset } from '../../arpLibrary';
import type { EndMode, InjectFolder } from '../constants';

// One arpeggiator's full control surface — state + every handler — for <Arpeggiator>. Mirrors App's
// ArpSlotT (arpSlot1 / arpSlot2 / the per-voice arpSlot).
export type ArpSlot = {
  arp: { on: boolean; pat: number; rate: number; oct: number; latch: boolean };
  mode: 'preset' | 'manual'; setMode: (m: 'preset' | 'manual') => void;
  presetId: string; activeName: string; seq: SeqStep[];
  play: () => void; stop: () => void; stepNav: (d: number) => void;
  selectPattern: (i: number) => void; applyPreset: (p: LibArpPreset) => void; applySeq: (st: SeqStep[]) => void;
  enterManual: () => void; resetManual: () => void;
  setRate: (i: number) => void; setOct: (n: number) => void; setLatch: (v: boolean) => void;
};

// A song player's current state (what the deck exposes to renderers).
export type PlayerState = { song: string; playing: boolean; name: string; prog: number };

// One MIDI-player "deck" — state + every handler — for <MidiPlayer>. Mirrors App's SongDeckT.
export type SongDeck = {
  v: number;                                            // 1-based synth voice this deck drives
  player: PlayerState; endMode: EndMode;
  vol: number; onVol: (n: number) => void; commitVol: (n: number) => void; volNote?: string;
  setSong: (name: string) => void;
  playFile: (arg: string, disp: string) => void;        // FolderBrowser tap: restart this file/baked song now
  play: () => void; stop: () => void; step: (dir: number) => void;
  applyEnd: (m: EndMode) => void; cycleEnd: () => void;
  onFolderList: (files: { arg: string; name: string }[]) => void;
  onNaturalEnd: () => void;
  browseRoot?: string; injectFolders?: InjectFolder[]; noEndMode?: boolean; filesFirst?: boolean;
};
