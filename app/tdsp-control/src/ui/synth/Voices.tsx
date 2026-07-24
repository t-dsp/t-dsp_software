// Voices.tsx — the Synth / Voices (instrument picker) feature, CARD + DETAIL PAGE in one file.
// (The user plans to rename this "Presets"; keeping "Voices" for now.)
//   • voicesActions(ctx, target) → the card's ‹ Prev / Next › instrument stepper
//   • voiceBrowserBody(ctx, target) → the detail page. Adapts to the track's engine:
//       – Plaits  → the full PlaitsPanel voice editor (model matrix + macros)
//       – OPLL / other picker engines → the engine's own fixed patch list
//       – Dexed   → the /dexed library: search box, breadcrumb folder nav, cart → 32 voices
//
// NOTE ON SHAPE: the Dexed browser is a stateful machine (folder path, cart load, DXLS search,
// scroll refs) whose state currently lives in App. This file is the extracted RENDERER — App owns
// the browse state and passes it in via VoicesCtx, so the JSX is editable here without moving ~15
// useState/useMemo/effects blind. A later pass can relocate that state machine into this file.
import React from 'react';
import { View, Text, Pressable, ScrollView, FlatList, TextInput, ActivityIndicator } from 'react-native';
import { C } from '../theme';
import { s } from '../styles';
import { HdrBtn, ListBtn } from '../primitives';
import { MediaBrowser } from '../browser/MediaBrowser';
import type { BrowserSource, BrowserFolder } from '../browser/MediaBrowser';
import { ROW_H, VItem, DexRow } from '../constants';
import PlaitsPanel from '../PlaitsPanel';
import type { Transport } from '../../transport';
import type { Catalog } from '../../catalog';

// Everything the voice browser reads out of App. Derived/ref-heavy bits are typed loosely (they're
// App-owned scroll refs / memoized browse state); App passes its real values straight through.
export type VoicesCtx = {
  tp: Transport; loaded: boolean; cat: Catalog;
  // engine dispatch
  trkEng: Record<number, string>; trkEngInstrs: Record<number, string[]>; trkNinstr: Record<number, number>;
  opllIdx: Record<number, number>; setOpllIdx: (u: (m: Record<number, number>) => Record<number, number>) => void;
  plaitsMacros: Record<number, any>; isPickerEngine: (eng?: string) => boolean;
  // selection + list/scroll refs (per target: 1 / 2 / >=3)
  selVoice: string; selVoice2: string; selVoiceX: Record<number, string>;
  voiceRef: any; voiceRef2: any; voiceRefX: any;
  browseRef: any; browseRef2: any; browseRefX: any; refFor: (refs: any, key: number) => any;
  pickVoice: (item: VItem, target: number) => void;
  // /dexed library search
  dexQuery: string; setDexQuery: (v: string) => void;
  dexResults: any; setDexResults: (v: any) => void;
  dexRows: DexRow[]; dexSearching: boolean; dexErr: string;
  pickDexRow: (item: DexRow, target: number) => void;
  // /dexed folder browse (App-derived)
  voiceData: VItem[]; pickerY: any; listId: string;
  libBusy: boolean; libErr: string;
  cart: { rel: string; name: string } | null; setCart: (v: { rel: string; name: string } | null) => void;
  vpath: string; setVpath: (v: string) => void;
  level: { folders: string[]; carts: { name: string; rel: string }[] };
  atRoot: boolean; goUp: () => void; crumbs: { label: string; go: () => void }[];
  // card stepper
  stepVoice: (dir: number, target: number) => void;
};

// The card's ‹ Prev / Next › instrument stepper (renders on the Voices tile + its detail header).
export const voicesActions = (ctx: VoicesCtx, target: number) => (<>
  <HdrBtn label="‹ Prev" stop onPress={() => ctx.stepVoice(-1, target)} />
  <HdrBtn label="Next ›" stop onPress={() => ctx.stepVoice(1, target)} />
</>);

export const voiceBrowserBody = (ctx: VoicesCtx, target: number) => {
  const {
    tp, loaded, cat, trkEng, trkEngInstrs, trkNinstr, opllIdx, setOpllIdx, plaitsMacros, isPickerEngine,
    selVoice, selVoice2, selVoiceX, voiceRef, voiceRef2, voiceRefX, browseRef, browseRef2, browseRefX, refFor,
    pickVoice, dexQuery, setDexQuery, dexResults, setDexResults, dexRows, dexSearching, dexErr, pickDexRow,
    voiceData, pickerY, listId, libBusy, libErr, cart, setCart, vpath, setVpath, level, atRoot, goUp, crumbs,
  } = ctx;
  const oi = target - 1;
  // PLAITS: a full voice EDITOR (model LED matrix + HARMONICS/TIMBRE/MORPH macros + LPG drawer) in
  // place of the flat patch list — the models still ride the same @TRK<i>.INSTR= path.
  if (trkEng[oi] === 'plaits') {
    const list = trkEngInstrs[oi] ?? [];
    return !list.length
      ? <View style={{ padding: 20, alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={[s.muted, { marginTop: 8 }]}>Loading Plaits models…</Text></View>
      : <PlaitsPanel models={list} ninstr={trkNinstr[oi] || list.length} curModel={opllIdx[oi] ?? 0} macros={plaitsMacros[oi]}
          onSelectModel={idx => { setOpllIdx(m => ({ ...m, [oi]: idx })); tp.trk(oi, 'INSTR=' + idx); tp.requestState(); }}
          onMacro={(field, permille) => tp.trk(oi, field + '=' + permille)} />;
  }
  // PICKER ENGINES (OPLL / YM2151 / OPL3 / Rings / VA / SF2 …): a FLAT patch list, no folder tree —
  // a single-pane <MediaBrowser> with a live filter over the engine's @TRK<i>.INSTRS names. Steps the
  // same @TRK<i>.INSTR= path the ‹/› card stepper uses.
  if (isPickerEngine(trkEng[oi])) {
    const list = trkEngInstrs[oi] ?? [];
    const cur = opllIdx[oi] ?? 0;
    const pickerSource: BrowserSource = {
      loading: !list.length,
      crumbs: [{ label: String(trkEng[oi]).toUpperCase() + ' voices', go: () => {} }],
      atRoot: true, goUp: () => {},
      folders: [],
      singlePane: true,
      selectedArg: 'e' + cur,
      items: list.map((it, i) => ({
        arg: 'e' + i,
        name: it.includes(': ') ? it.slice(it.indexOf(': ') + 2) : it,
        onPress: () => { setOpllIdx(m => ({ ...m, [oi]: i })); tp.trk(oi, 'INSTR=' + i); tp.requestState(); },
      })),
    };
    return <MediaBrowser source={pickerSource} />;
  }

  // DEXED: the /dexed cart LIBRARY as a two-pane <MediaBrowser> — folders + carts on the left rail,
  // the open cart's 32 voices (or the bundled set) on the right, plus device-driven @dxfind search.
  // The browse/search state machine lives in App (VoicesCtx); here we just ADAPT it to the generic
  // source contract (no logic moved), so Dexed looks and behaves like every other browser.
  if (!loaded) return <Text style={s.muted}>Connect to load voices.</Text>;
  const sel = target >= 3 ? (selVoiceX[target - 1] ?? '') : target === 2 ? selVoice2 : selVoice;
  const clearSearch = () => { setDexQuery(''); setDexResults(null); };
  // Left rail: the "★ Bundled voices" shelf (root only) + this folder's subfolders + its carts (the
  // open cart highlighted). Tapping a folder/cart mirrors the old ListBtn handlers exactly.
  const railFolders: BrowserFolder[] = [];
  if (vpath === '') railFolders.push({ key: '@bundled', label: 'Bundled voices (' + cat.instruments.length + ')', icon: '★', onPress: () => { setCart(null); setVpath('@bundled'); } });
  if (cat.hasDexed) {
    for (const f of level.folders) railFolders.push({ key: 'f/' + f, label: f, icon: '📁', onPress: () => { setCart(null); setVpath(vpath ? vpath + '/' + f : f); } });
    for (const c of level.carts) railFolders.push({ key: 'k/' + c.rel, label: c.name, icon: '🎛', active: cart?.rel === c.rel, onPress: () => setCart({ rel: c.rel, name: c.name }) });
  }
  const dexSource: BrowserSource = {
    loading: libBusy && !voiceData.length,
    crumbs, atRoot, goUp,
    folders: railFolders,
    selectedArg: sel,
    emptyItems: !cat.hasDexed ? 'No SD library found (/dexed empty?)'
      : libErr ? '⚠ ' + libErr
      : cart ? "Couldn't read this cart's voices." : 'Pick a bank or folder on the left to see its voices.',
    items: voiceData.map(d => ({ arg: d.key, name: d.label, onPress: () => pickVoice(d, target) })),
    search: {
      query: dexQuery, setQuery: setDexQuery, searching: dexSearching, error: dexErr, active: dexResults !== null,
      resultFolders: dexRows.filter(r => r.kind !== 'voice').map(r => r.kind === 'folder'
        ? { key: 'sf/' + r.path, label: r.name, icon: '📁', onPress: () => { pickDexRow(r, target); clearSearch(); } }
        : { key: 'sc/' + r.rel, label: r.name, icon: '🎛', onPress: () => { pickDexRow(r, target); clearSearch(); } }),
      resultItems: dexRows.filter((r): r is Extract<DexRow, { kind: 'voice' }> => r.kind === 'voice')
        .map(r => ({ arg: 'c' + r.rel + ':' + r.voice, name: r.vn + '   ·   ' + r.name, onPress: () => pickDexRow(r, target) })),
    },
  };
  return <MediaBrowser source={dexSource} />;
};
