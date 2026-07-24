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
  if (isPickerEngine(trkEng[oi])) {
    const list = trkEngInstrs[oi] ?? [];
    const cur = opllIdx[oi] ?? 0;
    return !list.length
      ? <View style={{ padding: 20, alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={[s.muted, { marginTop: 8 }]}>Loading {String(trkEng[oi]).toUpperCase()} voices…</Text></View>
      : <FlatList data={list} style={s.picker} nestedScrollEnabled keyExtractor={(_, i) => 'e' + i}
          getItemLayout={(_, index) => ({ length: ROW_H, offset: ROW_H * index, index })} onScrollToIndexFailed={() => {}}
          renderItem={({ item, index }) => <ListBtn label={item.includes(': ') ? item.slice(item.indexOf(': ') + 2) : item} sel={index === cur}
            onPress={() => { setOpllIdx(m => ({ ...m, [oi]: index })); tp.trk(oi, 'INSTR=' + index); tp.requestState(); }} />} />;
  }
  const sel = target >= 3 ? (selVoiceX[target - 1] ?? '') : target === 2 ? selVoice2 : selVoice;
  const lref = target >= 3 ? refFor(voiceRefX, target - 1) : target === 2 ? voiceRef2 : voiceRef;
  const bref = target >= 3 ? refFor(browseRefX, target - 1) : target === 2 ? browseRef2 : browseRef;
  return (
    <>
      {/* search box: matches folders, cart names, and voice names across ALL /dexed subfolders */}
      <View style={s.navBar}>
        <TextInput style={[s.input, { flex: 1 }]} value={dexQuery} onChangeText={setDexQuery}
          placeholder="Search all voices & folders…" placeholderTextColor={C.muted}
          autoCapitalize="none" autoCorrect={false} returnKeyType="search" />
        {dexQuery.length > 0 && (
          <Pressable style={s.upBtn} onPress={() => { setDexQuery(''); setDexResults(null); }}>
            <Text style={s.upTxt}>✕</Text>
          </Pressable>
        )}
      </View>
      {dexResults !== null ? (
        dexSearching && !dexRows.length ? (
          <View style={{ padding: 20, alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={[s.muted, { marginTop: 8 }]}>Searching library…</Text></View>
        ) : dexRows.length ? (
          <FlatList data={dexRows} style={s.picker} nestedScrollEnabled keyExtractor={(_, i) => 'sr' + i}
            renderItem={({ item }) => <ListBtn
              label={item.kind === 'folder' ? '📁 ' + item.name
                : item.kind === 'cart' ? '🎛 ' + item.name
                : '🎹 ' + item.vn + '   ·   ' + item.name}
              sel={item.kind === 'voice' && (target >= 3 ? (selVoiceX[target - 1] ?? '') : target === 2 ? selVoice2 : selVoice) === 'c' + item.rel + ':' + item.voice}
              onPress={() => pickDexRow(item, target)} />} />
        ) : (
          <Text style={[s.muted, { padding: 12 }]}>{dexErr ? '⚠ ' + dexErr : 'No matches for “' + dexQuery.trim() + '”.'}</Text>
        )
      ) : (
      <>
      {/* nav bar: up-one-level on the left, breadcrumb trail beside it */}
      <View style={s.navBar}>
        <Pressable style={[s.upBtn, atRoot && s.upBtnOff]} onPress={goUp} disabled={atRoot}>
          <Text style={s.upTxt}>‹</Text>
        </Pressable>
        <ScrollView horizontal style={s.crumbs} showsHorizontalScrollIndicator={false} contentContainerStyle={s.crumbsInner}>
          {crumbs.map((c, i) => {
            const last = i === crumbs.length - 1;
            return (
              <View key={i} style={s.crumbItem}>
                {i > 0 && <Text style={s.crumbSep}>›</Text>}
                <Pressable onPress={c.go} disabled={last}>
                  <Text style={last ? s.crumbLast : s.crumbTxt} numberOfLines={1}>{c.label}</Text>
                </Pressable>
              </View>
            );
          })}
        </ScrollView>
      </View>
      {/* the picker: always fills the remaining page height */}
      {!loaded ? <Text style={s.muted}>Connect to load voices.</Text> : voiceData.length ? (
        <FlatList ref={lref} data={voiceData} style={s.picker} nestedScrollEnabled keyExtractor={d => d.key}
          getItemLayout={(_, index) => ({ length: ROW_H, offset: ROW_H * index, index })}
          onScrollToIndexFailed={() => {}} scrollEventThrottle={32}
          onScroll={e => { pickerY.current[listId] = e.nativeEvent.contentOffset.y; }}
          renderItem={({ item }) => <ListBtn label={item.label} sel={sel === item.key} onPress={() => pickVoice(item, target)} />} />
      ) : libBusy ? (
        <View style={{ padding: 20, alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={[s.muted, { marginTop: 8 }]}>Loading…</Text></View>
      ) : cart ? (
        <Text style={s.muted}>Couldn't read this cart's voices.</Text>
      ) : (
        <ScrollView ref={bref} style={s.picker} nestedScrollEnabled scrollEventThrottle={32}
          onScroll={e => { pickerY.current[listId] = e.nativeEvent.contentOffset.y; }}>
          {vpath === '' && <ListBtn label={'★ Bundled voices (' + cat.instruments.length + ')'} onPress={() => setVpath('@bundled')} />}
          {cat.hasDexed && level.folders.map(f => <ListBtn key={'f' + f} label={'📁 ' + f} onPress={() => setVpath(vpath ? vpath + '/' + f : f)} />)}
          {cat.hasDexed && level.carts.map(c => <ListBtn key={c.rel} label={'🎛 ' + c.name} onPress={() => setCart({ rel: c.rel, name: c.name })} />)}
          {cat.hasDexed && !!libErr && <Text style={[s.muted, { padding: 12 }]}>⚠ SD library: {libErr} — restart the dev server with `expo start --web -c` and hard-reload.</Text>}
          {cat.hasDexed && !libErr && level.folders.length === 0 && level.carts.length === 0 && <Text style={s.muted}>{vpath === '' ? 'No SD library found (/dexed empty?)' : '(empty folder)'}</Text>}
        </ScrollView>
      )}
      </>
      )}
    </>
  );
};
