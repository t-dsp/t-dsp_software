// MediaBrowser.tsx — the componentized SD media browser that supersedes <FolderBrowser>. ONE reusable
// picker behind every "browse and pick a thing" surface: drum loops, MIDI songs, synth voices (Dexed
// library + picker-engine patch lists). Same look everywhere:
//   • TWO-PANE on a wide touchscreen — folders on the left, the open folder's items on the right —
//     collapsing to a single drill-in list when narrow (onLayout-measured) or when the source has no
//     folder hierarchy (a picker engine's flat patch list).
//   • SEARCH — a live local filter, or a device-driven recursive search when the source supplies one.
//   • FAVORITES + RECENTS — ★-star items + an auto-played shelf, pinned at the top of the rail (the
//     built-in @LS mode only; persisted per `scope` via ./store).
//   • RICHER ROWS — big touch targets, a ▶/•/♪ indicator, and a per-row star.
//
// It runs in one of two modes:
//   • BUILT-IN @LS mode (no `source`): browses the SD tree via tp.browse (drums, songs). Unchanged.
//   • SOURCE mode (`source` given): the caller supplies the folders/items/crumbs/search — used for the
//     Dexed voice library (@DXLS/@DXVL/@dxfind) and picker engines, whose data isn't an SD file tree.
// Both feed a single normalised view-model that the render consumes, so the UI is identical in both.
import React, { useEffect, useMemo, useState } from 'react';
import { View, Text, Pressable, ScrollView, TextInput, ActivityIndicator } from 'react-native';
import { C } from '../theme';
import { s } from '../styles';
import { InjectFolder, stripExt } from '../constants';
import type { Transport } from '../../transport';
import { sortEntries } from '../../browse';
import type { BrowseEntry } from '../../browse';
import { useBrowserLists, BrowserItem } from './store';

// A left-rail (or search-result) folder row. Carries its OWN tap action so the built-in and source
// modes can each wire navigation however they like; `active` highlights the open folder/shelf.
export type BrowserFolder = { key: string; label: string; icon?: string; onPress: () => void; active?: boolean };

// SOURCE mode contract: everything the browser needs when the data ISN'T an SD file tree (voices).
export type BrowserSource = {
  loading: boolean;
  crumbs: { label: string; go: () => void }[];
  atRoot: boolean;
  goUp: () => void;
  folders: BrowserFolder[];               // left-rail folders (empty + singlePane ⇒ one column)
  items: BrowserItem[];                   // right-pane items (each may carry its own onPress)
  onItem?: (it: BrowserItem) => void;     // fallback tap action for items with no onPress
  selectedArg?: string;                   // highlight the current pick
  playingArg?: string;
  singlePane?: boolean;                   // no folder hierarchy → single column (picker engines)
  emptyItems?: string;                    // message when the item pane is empty
  // Device-driven recursive search (Dexed @dxfind). Omit ⇒ the browser filters `items` locally.
  // `active` lets the source decide when results TAKE OVER (Dexed only searches at ≥2 chars, so a
  // 1-char query keeps showing the folder browser); defaults to "query is non-empty".
  search?: {
    query: string; setQuery: (q: string) => void; searching: boolean; error?: string; active?: boolean;
    resultFolders: BrowserFolder[]; resultItems: BrowserItem[];
  };
};

const WIDE_MIN = 620;   // px of pane width at/above which the two-column layout turns on

export function MediaBrowser({
  tp, root, ext, enabled, selected, playing, onSelectFile, injectFolders, onFolderList, scope, accent = C.accent, source,
}: {
  tp?: Transport; root?: string; ext?: string; enabled?: boolean;   // @LS mode (ignored when `source` is set)
  selected?: string; playing?: string;
  onSelectFile?: (fullPath: string, displayName: string) => void;
  injectFolders?: InjectFolder[];
  onFolderList?: (files: BrowserItem[]) => void;   // publish the in-view item list (header ‹/› steps through it)
  scope?: string;      // favorites/recents namespace; defaults to the root path
  accent?: string;     // section accent for the play indicator / active highlights
  source?: BrowserSource;   // SOURCE mode: caller supplies the data (voices)
}) {
  const [wide, setWide] = useState(true);   // onLayout-measured: two-pane vs single drill

  // ---------------------------------------------------------------------------------------------
  // BUILT-IN @LS state (only meaningful when `source` is absent). Kept at the top level so hooks run
  // unconditionally; when a `source` is given these simply idle.
  const [path, setPath] = useState(root ?? '/');
  const [virt, setVirt] = useState<string | null>(null);
  const [entries, setEntries] = useState<BrowseEntry[] | null>(null);
  const [err, setErr] = useState('');
  const [lquery, setLquery] = useState('');
  const { favs, recents, isFav, toggleFav, addRecent } = useBrowserLists(scope ?? root ?? 'default');

  useEffect(() => { setPath(root ?? '/'); setVirt(null); setLquery(''); }, [root]);

  useEffect(() => {
    if (source || !tp || !enabled) { if (!source) setEntries(null); return; }
    let alive = true;
    setEntries(null); setErr('');
    tp.browse(path, ext)
      .then(r => { if (alive) setEntries(sortEntries(r.entries)); })
      .catch(e => { if (alive) { setEntries([]); setErr(String((e as any)?.message || e || 'browse failed')); } });
    return () => { alive = false; };
  }, [path, ext, enabled, tp, source]);

  const lsVirtuals = useMemo(() => {
    const out: { key: string; label: string; icon: string; leaves: BrowserItem[] }[] =
      (injectFolders || []).map(f => ({ key: '@' + f.name, label: f.name.replace(/^★\s*/, ''), icon: '★', leaves: f.leaves }));
    if (favs.length) out.push({ key: '@favorites', label: 'Favorites', icon: '⭐', leaves: favs });
    if (recents.length) out.push({ key: '@recent', label: 'Recent', icon: '🕘', leaves: recents });
    return out;
  }, [injectFolders, favs, recents]);

  const lsItems = useMemo<BrowserItem[]>(() => {
    if (virt) return lsVirtuals.find(v => v.key === virt)?.leaves ?? [];
    if (!entries) return [];
    return entries.filter(e => e.type === 'F').map(e => ({ arg: path + '/' + e.name, name: stripExt(e.name, ext) }));
  }, [virt, lsVirtuals, entries, path, ext]);

  // Publish the in-view item list up to the deck so the header ‹/› step through exactly what's shown
  // (@LS mode only; skip the transient empty during a folder fetch so a mid-load ‹/› still works).
  useEffect(() => {
    if (source) return;
    if (!virt && entries === null) return;
    onFolderList?.(lsItems);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [lsItems, virt, entries, source]);

  const lsAtRoot = path === (root ?? '/');
  const lsGoUp = () => { if (virt) { setVirt(null); return; } if (path !== (root ?? '/')) setPath(path.split('/').slice(0, -1).join('/') || '/'); };
  const enterFolder = (name: string) => { setVirt(null); setLquery(''); setPath(path + '/' + name); };

  const lsCrumbs = useMemo(() => {
    const r = root ?? '/';
    const rootName = r.split('/').pop() || r;
    const rel = path.startsWith(r) ? path.slice(r.length).replace(/^\//, '') : '';
    const segs = rel ? rel.split('/') : [];
    const cr: { label: string; go: () => void }[] = [{ label: rootName, go: () => { setVirt(null); setPath(r); } }];
    let acc = r; for (const seg of segs) { acc += '/' + seg; const p = acc; cr.push({ label: seg, go: () => { setVirt(null); setPath(p); } }); }
    if (virt) cr.push({ label: lsVirtuals.find(v => v.key === virt)?.label ?? '', go: () => {} });
    return cr;
  }, [root, path, virt, lsVirtuals]);

  // ---------------------------------------------------------------------------------------------
  // Normalised view-model — the render reads ONLY this, so @LS and source modes look identical.
  const query = source?.search ? source.search.query : lquery;
  const setQuery = source?.search ? source.search.setQuery : setLquery;
  const q = query.trim().toLowerCase();

  const vm = source ? {
    loading: source.loading,
    crumbs: source.crumbs,
    atRoot: source.atRoot,
    goUp: source.goUp,
    shortcuts: [] as BrowserFolder[],
    folders: source.folders,
    items: source.items,
    onItem: source.onItem,
    isSel: (arg: string) => source.selectedArg === arg,
    isPlaying: (arg: string) => source.playingArg === arg,
    fav: undefined as undefined | { isFav: (a: string) => boolean; toggle: (it: BrowserItem) => void },
    singlePane: !!source.singlePane,
    emptyItems: source.emptyItems ?? '',
    searching: !!source.search?.searching,
    searchError: source.search?.error,
    deviceSearch: !!source.search,
    resultFolders: source.search?.resultFolders ?? [],
    resultItems: source.search?.resultItems ?? [],
  } : {
    loading: !!enabled && !virt && entries === null,
    crumbs: lsCrumbs,
    atRoot: lsAtRoot && !virt,
    goUp: lsGoUp,
    shortcuts: lsVirtuals.map(v => ({ key: v.key, label: v.label, icon: v.icon, active: virt === v.key, onPress: () => { setVirt(v.key); setLquery(''); } })),
    folders: (entries || []).filter(e => e.type === 'D').map(e => ({ key: 'd/' + e.name, label: e.name, icon: '📁', onPress: () => enterFolder(e.name) })),
    items: lsItems,
    onItem: (it: BrowserItem) => { addRecent(it); onSelectFile?.(it.arg, it.name); },
    isSel: (arg: string) => selected === arg,
    isPlaying: (arg: string) => playing === arg,
    fav: { isFav, toggle: toggleFav },
    singlePane: false,
    emptyItems: virt ? '(empty)' : '(no items here)',
    searching: false,
    searchError: err,
    deviceSearch: false,
    resultFolders: [] as BrowserFolder[],
    resultItems: [] as BrowserItem[],
  };

  const searchActive = source?.search ? (source.search.active ?? q.length > 0) : q.length > 0;
  // What the item pane shows: device results while searching (Dexed), a local filter otherwise.
  const shownItems = searchActive
    ? (vm.deviceSearch ? vm.resultItems : vm.items.filter(it => it.name.toLowerCase().includes(q)))
    : vm.items;
  const twoPane = wide && !vm.singlePane;

  // ---- row renderers --------------------------------------------------------------------------
  const folderRow = (f: BrowserFolder) => (
    <Pressable key={f.key} onPress={f.onPress} style={[s.brRow, f.active && s.brRowActive]}>
      <Text style={s.brRowIcon}>{f.icon ?? '📁'}</Text>
      <Text style={s.brRowName} numberOfLines={1}>{f.label}</Text>
      <Text style={s.brChevron}>›</Text>
    </Pressable>
  );
  const itemRow = (it: BrowserItem) => {
    const isPlaying = vm.isPlaying(it.arg);
    const isSel = vm.isSel(it.arg);
    const fav = vm.fav?.isFav(it.arg);
    const onPress = it.onPress ?? (vm.onItem ? () => vm.onItem!(it) : undefined);
    return (
      <Pressable key={it.arg} onPress={onPress} style={[s.brRow, isSel && s.brRowSel]}>
        <Text style={[s.brRowIcon, { color: isPlaying ? accent : C.muted }]}>{isPlaying ? '▶' : isSel ? '•' : '♪'}</Text>
        <Text style={[s.brRowName, isPlaying && { color: accent, fontWeight: '700' }]} numberOfLines={1}>{it.name}</Text>
        {vm.fav && (
          <Pressable onPress={() => vm.fav!.toggle(it)} hitSlop={10} style={s.brStar}>
            <Text style={[s.brStarTxt, fav && { color: '#e3b341' }]}>{fav ? '★' : '☆'}</Text>
          </Pressable>
        )}
      </Pressable>
    );
  };

  const navBar = (
    <View style={s.navBar}>
      <Pressable style={[s.upBtn, vm.atRoot && s.upBtnOff]} onPress={vm.goUp} disabled={vm.atRoot}>
        <Text style={s.upTxt}>‹</Text>
      </Pressable>
      <ScrollView horizontal style={s.crumbs} showsHorizontalScrollIndicator={false} contentContainerStyle={s.crumbsInner}>
        {vm.crumbs.map((c, i) => {
          const last = i === vm.crumbs.length - 1;
          return (
            <View key={i} style={s.crumbItem}>
              {i > 0 && <Text style={s.crumbSep}>›</Text>}
              <Pressable onPress={c.go} disabled={last} style={[s.crumbBtn, last && s.crumbBtnLast]}>
                <Text style={last ? s.crumbLast : s.crumbTxt} numberOfLines={1}>{c.label}</Text>
              </Pressable>
            </View>
          );
        })}
      </ScrollView>
    </View>
  );

  const searchBar = (
    <View style={s.navBar}>
      <TextInput style={[s.input, { flex: 1 }]} value={query} onChangeText={setQuery}
        placeholder={vm.deviceSearch ? 'Search all voices & folders…' : 'Search…'} placeholderTextColor={C.muted}
        autoCapitalize="none" autoCorrect={false} returnKeyType="search" />
      {query.length > 0 && <Pressable style={s.upBtn} onPress={() => setQuery('')}><Text style={s.upTxt}>✕</Text></Pressable>}
    </View>
  );

  // The right-pane item list (with the device-search folder/cart rows floated on top while searching).
  const itemList = (
    <ScrollView style={s.brScroll} nestedScrollEnabled>
      {vm.loading || (searchActive && vm.searching && !shownItems.length && !vm.resultFolders.length) ? (
        <View style={s.brLoad}><ActivityIndicator color={accent} /><Text style={[s.muted, { marginTop: 8 }]}>{vm.searching ? 'Searching…' : 'Loading…'}</Text></View>
      ) : (
        <>
          {searchActive && vm.deviceSearch && vm.resultFolders.map(folderRow)}
          {shownItems.length ? shownItems.map(itemRow)
            : !(searchActive && vm.deviceSearch && vm.resultFolders.length)
              && <Text style={[s.muted, { padding: 14 }]}>{searchActive ? 'No matches for “' + query.trim() + '”.' : vm.emptyItems}</Text>}
        </>
      )}
      {!!vm.searchError && !searchActive && <Text style={[s.muted, { padding: 12 }]}>⚠ {vm.searchError}</Text>}
    </ScrollView>
  );

  if (source ? false : !enabled) {
    return <View style={s.brWrap} onLayout={e => setWide(e.nativeEvent.layout.width >= WIDE_MIN)}><Text style={s.muted}>Connect to browse files.</Text></View>;
  }

  return (
    <View style={s.brWrap} onLayout={e => setWide(e.nativeEvent.layout.width >= WIDE_MIN)}>
      {twoPane ? (
        <View style={s.brPanes}>
          <View style={[s.brPane, s.brLeft]}>
            <View style={s.brPaneHead}>{navBar}</View>
            <ScrollView style={s.brScroll} nestedScrollEnabled>
              {vm.shortcuts.length > 0 && <><Text style={s.brSection}>SHORTCUTS</Text>{vm.shortcuts.map(folderRow)}</>}
              <Text style={s.brSection}>FOLDERS</Text>
              {vm.loading ? <View style={s.brLoad}><ActivityIndicator color={accent} /></View>
                : vm.folders.length ? vm.folders.map(folderRow)
                : <Text style={[s.muted, { padding: 12 }]}>{vm.atRoot ? '(no folders)' : '(no subfolders)'}</Text>}
            </ScrollView>
          </View>
          <View style={[s.brPane, s.brRight]}>
            <View style={s.brPaneHead}>{searchBar}</View>
            {itemList}
          </View>
        </View>
      ) : (
        <View style={[s.brPane, { flex: 1 }]}>
          <View style={s.brPaneHead}>{!vm.singlePane && navBar}{searchBar}</View>
          <ScrollView style={s.brScroll} nestedScrollEnabled>
            {!searchActive && !vm.singlePane && vm.shortcuts.map(folderRow)}
            {!searchActive && !vm.singlePane && vm.folders.map(folderRow)}
            {searchActive && vm.deviceSearch && vm.resultFolders.map(folderRow)}
            {vm.loading || (searchActive && vm.searching && !shownItems.length) ? (
              <View style={s.brLoad}><ActivityIndicator color={accent} /><Text style={[s.muted, { marginTop: 8 }]}>{vm.searching ? 'Searching…' : 'Loading…'}</Text></View>
            ) : shownItems.length ? shownItems.map(itemRow)
              : <Text style={[s.muted, { padding: 14 }]}>{searchActive ? 'No matches.' : vm.singlePane ? '' : vm.emptyItems}</Text>}
            {!!vm.searchError && !searchActive && <Text style={[s.muted, { padding: 12 }]}>⚠ {vm.searchError}</Text>}
          </ScrollView>
        </View>
      )}
    </View>
  );
}
