// primitives.tsx — the leaf, prop-driven UI building blocks of the T-DSP control surface.
// Extracted verbatim from App.tsx (Phase 1 split): every component here is self-contained (it
// takes props, holds at most its own local state) and is composed by App and by each section's
// body. Depends only on the theme palette, the shared StyleSheet, the pure constants/helpers, and
// the transport/catalog/browse types — never back on App, so there's no import cycle.
import React, { useState, useRef, useEffect } from 'react';
import { View, Text, Pressable, ScrollView, FlatList, ActivityIndicator, useWindowDimensions } from 'react-native';
import Slider from '@react-native-community/slider';
import { C } from './theme';
import { s } from './styles';
import { kb, stripExt, InjectFolder } from './constants';
import type { LoadProgress } from '../catalog';
import type { Transport } from '../transport';
import { sortEntries } from '../browse';
import type { BrowseEntry } from '../browse';

// The subtitle line under a section title: the live accent value, else a muted status tag.
export const Subtitle = ({ value, status }: { value?: string; status?: string }) =>
  !!value ? <Text style={s.drawerValue} numberOfLines={1}>{value}</Text>
    : !!status ? <Text style={s.tag}>{status}</Text> : null;

// A slim 0..1 progress bar (song playback position). Sits right under the track title.
export const ProgressBar = ({ value }: { value: number }) => (
  <View style={s.progTrack}><View style={[s.progFill, { width: `${Math.max(0, Math.min(1, value)) * 100}%` }]} /></View>
);

// Loop-recorder step grid: `bars` groups of `sig` beat cells (the master meter). As a take records or
// plays, cells fill left→right up to the live loop position (prog 0..1), the current step glows, and
// each bar's downbeat is marked — so you can watch the take march to the end of the last bar and wrap.
// recording=true tints it red (capturing), else green (playback). active=false shows the empty grid as
// a preview of the chosen length. Driven by the device's loop-position feed (prog), so it's grid-accurate.
export const LoopStepGrid = ({ bars, sig, prog, active, recording }:
  { bars: number; sig: number; prog: number; active: boolean; recording: boolean }) => {
  const nBars = Math.max(1, bars | 0);
  const nSig = Math.max(1, Math.min(16, sig | 0));
  const total = nBars * nSig;
  const p = Math.max(0, Math.min(0.99999, prog));
  const cur = active ? Math.min(total - 1, Math.floor(p * total)) : -1;
  return (
    <View style={s.stepGrid}>
      {Array.from({ length: nBars }).map((_, b) => (
        <View key={b} style={s.stepBar}>
          {Array.from({ length: nSig }).map((_, k) => {
            const i = b * nSig + k;
            const past = active && i < cur;
            const on = active && i === cur;
            return <View key={k} style={[
              s.stepCell,
              k === 0 && s.stepCellDown,
              past && (recording ? s.stepCellRecPast : s.stepCellPast),
              on && (recording ? s.stepCellRecOn : s.stepCellOn),
            ]} />;
          })}
        </View>
      ))}
    </View>
  );
};

// Header beat lights: one circle per beat of the bar, the current beat lit, beat 1 (the
// downbeat / accent) in a distinct color. Two clock sources, preferring the device:
//   • LIVE (`live`): the device's @BEAT feed — locked to the REAL master downbeat + meter.
//   • LOCAL fallback: a self-correcting clock at the master BPM, for firmware that doesn't
//     emit @BEAT. Matches the tempo but re-anchors its downbeat to "now" on tempo/meter change.
// Only this component re-renders each beat (its own state), not the whole app.
export function BeatStrip({ sig, bpm, active, live }: { sig: number; bpm: number; active: boolean; live: { i: number; n: number } | null }) {
  const [beat, setBeat] = useState(-1);
  const anchor = useRef(0);
  const useLive = active && !!live;
  const beats = useLive ? live!.n : sig;   // device meter when live, else the @METROSIG setting
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
  if (!active || beats < 1) return null;
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

// Homepage card: the section's title, live value, and header controls. Fixed size so
// every card matches. Title/value sit on top; controls always sit on their own row at
// the bottom (a card is far narrower than the window, so they never share the title's
// line). Only the › chevron opens the section — the card body itself is inert, so the
// header controls (nested Pressables) never risk a stray navigation.
// The SAME Card is used on the home grid AND as a section-page header (pass `onBack`): identical
// content (title/subtitle/foot/actions), just a left back-arrow instead of the right open-chevron.
export function Card({ title, value, status, subtitle, actions, foot, progress, onPress, onBack, style, accent, tint, topRight, disabledReason }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; foot?: React.ReactNode; progress?: number; onPress?: () => void; onBack?: () => void; style?: any; accent?: string; tint?: string; topRight?: React.ReactNode; disabledReason?: string }) {
  // A feature the firmware compiled in but that can't run right now (e.g. a PSRAM-only synth/looper/fx
  // on a board without PSRAM, or a missing SD asset): grey the whole card, swap the subtitle for the
  // reason, and make it inert (no chevron, no header actions) — the box is visible so the user knows
  // it exists, with the reason shown, but it can't be opened or driven. See @STATE "unavail".
  const off = !!disabledReason;
  return (
    <View style={[s.card, style, tint && { backgroundColor: tint }, accent && { borderLeftColor: accent, borderLeftWidth: 3 }, off && s.cardOff]}>
      <View style={s.cardHead}>
        {onBack && <Pressable onPress={onBack} hitSlop={10} style={s.chevBtn}><Text style={s.chev}>‹</Text></Pressable>}
        <View style={s.drawerLeft}>
          <Text style={[s.drawerTitle, accent && { color: accent }]} numberOfLines={1}>{title}</Text>
          {off ? <Text style={s.cardReason} numberOfLines={1}>⚠  {disabledReason}</Text>
            : subtitle != null
              // A plain-string subtitle must be wrapped in styled <Text> — a bare string lands unstyled
              // (near-black default) and vanishes on the dark card. JSX subtitles pass through as-is.
              ? (typeof subtitle === 'string' ? <Text style={s.drawerValue} numberOfLines={1}>{subtitle}</Text> : subtitle)
              : <Subtitle value={value} status={status} />}
          {!off && progress != null && <ProgressBar value={progress} />}
        </View>
        {!off && topRight}
        {!onBack && <Pressable onPress={off ? undefined : onPress} disabled={off} hitSlop={10} style={s.chevBtn}><Text style={[s.chev, off && { opacity: 0 }]}>❯</Text></Pressable>}
      </View>
      {!off && !!foot && <View style={s.cardFoot}>{foot}</View>}
      {!off && !!actions && <View style={s.cardActions}>{actions}</View>}
    </View>
  );
}

// Section page header: a back arrow + the section title/value, with the same controls
// available (on the right when wide, on their own row when narrow).
export function PageHeader({ title, value, status, subtitle, actions, progress, onBack, accent, tint, topRight }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; progress?: number; onBack: () => void; accent?: string; tint?: string; topRight?: React.ReactNode }) {
  const { width } = useWindowDimensions();
  const narrow = width < 640;
  return (
    <View style={[s.pageHead, tint && { backgroundColor: tint }, accent && { borderBottomColor: accent }]}>
      <View style={s.pageHeadRow}>
        <View style={s.drawerLeft}>
          <Text style={[s.pageTitle, accent && { color: accent }]}>{title}</Text>
          {/* A plain-string subtitle must be wrapped in <Text> — a bare string as a View child throws on
              react-native-web ("text node cannot be a child of a View"). JSX subtitles pass through. */}
          {subtitle != null
            ? (typeof subtitle === 'string' ? <Text style={s.drawerValue} numberOfLines={1}>{subtitle}</Text> : subtitle)
            : <Subtitle value={value} status={status} />}
          {progress != null && <ProgressBar value={progress} />}
        </View>
        {topRight}
        {!narrow && !!actions && <View style={s.headActions}>{actions}</View>}
        {/* Back sits at the top-right of the page header (title/value stay on the left). */}
        <Pressable style={s.backBtn} onPress={onBack}><Text style={s.backTxt}>❮</Text></Pressable>
      </View>
      {narrow && !!actions && <View style={s.hdrActionsRow}>{actions}</View>}
    </View>
  );
}

// A tiny pub/sub for catalog-load progress. The load emits ~10 updates/sec; if that drove
// App-level state it would re-render the ENTIRE app that often, and those synchronous renders
// starve the USB reader loop enough to stall the very transfer we're showing (a self-inflicted
// hang — see catalog.ts throttle note). So progress flows through this bus to LoadScreen ONLY,
// which owns its own state and re-renders in isolation. `last` lets a fresh subscriber catch up.
export class ProgressBus {
  private listeners = new Set<(p: LoadProgress | null) => void>();
  private last: LoadProgress | null = null;
  emit = (p: LoadProgress | null) => { this.last = p; this.listeners.forEach(l => l(p)); };
  get value() { return this.last; }
  subscribe(l: (p: LoadProgress | null) => void) { this.listeners.add(l); l(this.last); return () => { this.listeners.delete(l); }; }   // replay latest so a subscriber can't miss the mount→effect gap
}

// The "Loading catalog…" screen. Owns progress + elapsed-seconds state locally (fed by the
// ProgressBus) so its 10x/sec updates re-render this component alone, not the whole App.
export function LoadScreen({ bus, tpLabel }: { bus: ProgressBus; tpLabel: string }) {
  const [prog, setProg] = useState<LoadProgress | null>(bus.value);
  const [elapsed, setElapsed] = useState(0);
  useEffect(() => bus.subscribe(setProg), [bus]);
  // Ticks while this screen is mounted (i.e. connected but not yet loaded), so the load reads
  // as "working" even if a single @READ stalls — a frozen bar looks broken.
  useEffect(() => {
    const t0 = Date.now();
    const id = setInterval(() => setElapsed(Math.floor((Date.now() - t0) / 1000)), 1000);
    return () => clearInterval(id);
  }, []);
  const pct = prog && prog.total > 0 ? Math.min(100, Math.round(100 * prog.done / prog.total)) : 0;
  return (
    <View style={s.loadWrap}>
      <ActivityIndicator color={C.accent} size="large" />
      <Text style={s.loadTitle}>Loading catalog…</Text>
      {prog && prog.index > 0 && prog.det && prog.total > 0 ? (
        // A file is streaming and the device reported sizes: live byte-fraction bar.
        <>
          <View style={s.loadTrack}><View style={[s.loadFill, { width: `${pct}%` }]} /></View>
          <Text style={s.loadSub}>{prog.label} · {prog.index}/{prog.count} · {pct}% · {kb(prog.done)}/{kb(prog.total)} KB</Text>
        </>
      ) : (
        // Reading the index, or old firmware with no sizes: name the step instead.
        <Text style={s.loadSub}>{prog && prog.index > 0 ? `${prog.label} · ${prog.index}/${prog.count}` : 'Reading catalog index…'}</Text>
      )}
      <Text style={s.loadHint}>{elapsed}s elapsed{elapsed >= 6 ? ` · streaming over ${tpLabel}…` : ''}</Text>
    </View>
  );
}

// A transport button for a section header (nested Pressable → doesn't navigate the card).
// All header buttons share one uniform width (s.hdrBtn.minWidth).
// `active` (play buttons only): true = playing (green), false = idle (dark). Omit on non-play
// buttons (nav/stop/±) so they keep their normal look.
export const HdrBtn = ({ label, onPress, stop, active, style }: { label: string; onPress: () => void; stop?: boolean; active?: boolean; style?: any }) => (
  <Pressable onPress={onPress} style={[s.hdrBtn, stop && s.hdrBtnStop, active === false && s.hdrBtnIdle, style]}><Text style={s.hdrBtnText} numberOfLines={1}>{label}</Text></Pressable>
);
// A small keyboard glyph drawn with Views so it can be tinted (an emoji can't): a bordered
// body with five keys. WHITE = this synth owns the USB keyboard; GREY = another synth does.
export function KbdGlyph({ color }: { color: string }) {
  return (
    <View style={{ width: 26, height: 17, borderWidth: 1.5, borderColor: color, borderRadius: 3, paddingHorizontal: 2.5, paddingBottom: 2.5, flexDirection: 'row', alignItems: 'flex-end', justifyContent: 'space-between' }}>
      {[0, 1, 2, 3, 4].map(i => <View key={i} style={{ width: 2.5, height: 7, backgroundColor: color, borderRadius: 1 }} />)}
    </View>
  );
}
// Keyboard-ownership control: tap a GREY keyboard to route the USB keyboard to this synth.
export const KbdBtn = ({ owned, onPress }: { owned: boolean; onPress: () => void }) => (
  <Pressable onPress={onPress} hitSlop={10} style={s.kbdBtn}
    accessibilityLabel={owned ? 'USB keyboard plays this synth' : 'Tap to play this synth with the USB keyboard'}>
    <KbdGlyph color={owned ? C.text : C.muted} />
  </Pressable>
);
export const Row = ({ children }: any) => <View style={s.row}>{children}</View>;
// Live-drag command rate. Sliders now send WHILE dragging (not just on release) so the change is
// heard as you move — throttled to ~1 msg / SLIDER_TX_MS to stay inside the serial/BLE budget (the
// ESP32 relay throttles at TX_GAP_MS=20ms and drops the queue tail past ~25 msg/s; see
// project_serial_bridge_throttle). 60 ms ≈ 16 sends/s: the thumb tracks the finger at 60 fps
// (onValueChange) while the device gets a smooth stream of small step=1 updates — continuous to the
// ear without bursting the wire. (If a specific param still zippers, add firmware-side param
// smoothing for it — decouples audio smoothness from the send rate.)
const SLIDER_TX_MS = 60;
// Slider that streams throttled onCommit()s during the drag: leading edge (immediate) + a trailing
// timer so a HELD position still lands within SLIDER_TX_MS, and the exact final value on release.
export const ThrottledSlider = ({ value, onChange, onCommit, min = 0, max = 150, step = 1, disabled, style }:
  { value: number; onChange: (v: number) => void; onCommit: (v: number) => void;
    min?: number; max?: number; step?: number; disabled?: boolean; style?: any }) => {
  const last = useRef(0); const pend = useRef<number | null>(null); const timer = useRef<any>(null);
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); }, []);
  const flush = () => {
    if (timer.current) { clearTimeout(timer.current); timer.current = null; }
    if (pend.current !== null) { last.current = Date.now(); const v = pend.current; pend.current = null; onCommit(v); }
  };
  const change = (v: number) => {
    onChange(v);                                   // move the thumb/label immediately (60fps, no send)
    pend.current = v;
    const dt = Date.now() - last.current;
    if (dt >= SLIDER_TX_MS) flush();               // leading edge
    else if (!timer.current) timer.current = setTimeout(flush, SLIDER_TX_MS - dt);   // trailing edge
  };
  return (
    <Slider style={style ?? { flex: 1, height: 34 }} minimumValue={min} maximumValue={max} step={step} value={value}
      minimumTrackTintColor={C.accent} maximumTrackTintColor={C.border} thumbTintColor={C.accent}
      disabled={disabled} onValueChange={change} onSlidingComplete={v => { pend.current = v; flush(); }} />
  );
};
// A per-section level slider (0..150 %, 100 = the file's own velocity), independent of the master
// @VOL. Streams throttled sends during the drag (ThrottledSlider) so you hear it ramp as you move.
export const VolSlider = ({ label, value, onChange, onCommit, disabled, max = 150 }:
  { label: string; value: number; onChange: (v: number) => void; onCommit: (v: number) => void; disabled?: boolean; max?: number }) => (
  <View style={s.volRow}>
    <Text style={[s.muted, { width: 52 }]}>{label}</Text>
    <ThrottledSlider value={value} onChange={onChange} onCommit={onCommit} max={max} disabled={disabled} />
    <Text style={[s.muted, { width: 44, textAlign: 'right' }]}>{Math.round(value)}%</Text>
  </View>
);
export const Stat = ({ label, n, sub }: { label: string; n: number; sub?: string }) => (
  <View style={s.stat}><Text style={s.statN}>{n}</Text><Text style={s.statL}>{label}</Text>{!!sub && <Text style={s.statSub}>{sub}</Text>}</View>
);
export const ListBtn = ({ label, sel, onPress }: any) => (
  <Pressable onPress={onPress} style={[s.listBtn, sel && s.listBtnSel]}><Text style={s.text} numberOfLines={1}>{label}</Text></Pressable>
);
// A tab strip INSIDE a page body (e.g. a MIDI player's Player / Looper split). This is not the
// old page-level tab nav — the router still uses menu > submenu; these just split one card's own
// content. Tab state is local, so switching never touches the route. Reuses the arp's tab styling.
export function BodyTabs({ tabs }: { tabs: { key: string; label: string; body: React.ReactNode }[] }) {
  const [active, setActive] = useState(tabs[0]?.key);
  const cur = tabs.find(t => t.key === active) || tabs[0];
  return (
    <View style={{ gap: 10 }}>
      <View style={s.arpTabs}>
        {tabs.map(t => (
          <Pressable key={t.key} style={[s.arpTab, active === t.key && s.arpTabOn]} onPress={() => setActive(t.key)}>
            <Text style={[s.arpTabTxt, active === t.key && s.arpTabTxtOn]}>{t.label}</Text>
          </Pressable>
        ))}
      </View>
      {cur?.body}
    </View>
  );
}
// A submenu page: full-width cards (one per row) for a parent section's children (e.g. Settings →
// Connection, TAC5212). Tapping a card opens that child's own existing page via onOpen(id).
// `getItems` is a getter so it can read the sections array lazily (it isn't assigned yet when the
// body is built).
// `accent`/`tint`, when given, override each child's own colors so ALL sub-cards match the parent
// tile — a quick visual cue for which section (e.g. which synthesizer) you're inside.
export function SubMenu({ getItems, onOpen, accent, tint }: { getItems: () => any[]; onOpen: (id: string) => void; accent?: string; tint?: string }) {
  // Tile the children into a responsive grid rather than a stacked column. Columns come from the
  // CONTAINER width (a section page is capped well below the window, so useWindowDimensions would
  // over-count) — measured via onLayout, same 1/2/3-column feel as the home grid. Each card keeps its
  // fixed cardGrid size so the grid reads as uniform tiles.
  const [w, setW] = useState(0);
  const cols = w >= 900 ? 3 : w >= 560 ? 2 : 1;
  return (
    <View style={s.submenu} onLayout={e => setW(e.nativeEvent.layout.width)}>
      {getItems().map(sec => (
        <View key={sec.id} style={[s.submenuCell, { width: `${100 / cols}%` }]}>
          <Card title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} actions={sec.actions} foot={sec.foot}
            onPress={() => onOpen(sec.id)} style={s.cardGrid} accent={accent ?? sec.accent} tint={tint ?? sec.tint} topRight={sec.topRight} disabledReason={sec.disabledReason} />
        </View>
      ))}
    </View>
  );
}

// ---- <FolderBrowser> : the generic recursive SD file/folder browser (the @LS client) -------
// One reusable component behind the MIDI-player and Drums file pickers (and, later, any SD tree
// — samples/soundfonts). Drills folder-by-folder from `root` via transport.browse() (@LS), lists
// files filtered by `ext` (sorted client-side by ./browse.sortEntries), and calls onSelectFile
// with the FULL SD path + a display name. Mirrors the Voices browser's breadcrumb + up-button UX
// and reuses ListBtn. `injectFolders` adds synthetic folders at the ROOT level whose leaves play
// by their own arg (used for the baked "tests" songs, which live in flash, not on the card).
// `enabled` gates the live fetch (connected + catalog loaded). `selected`/`playing` are the
// currently-chosen / currently-playing arg (full path or a baked name) for row highlighting.
export function FolderBrowser({ tp, root, ext, enabled, selected, playing, onSelectFile, injectFolders, onFolderList, filesFirst }: {
  tp: Transport; root: string; ext?: string; enabled: boolean;
  selected?: string; playing?: string;
  onSelectFile: (fullPath: string, displayName: string) => void;
  injectFolders?: InjectFolder[];
  onFolderList?: (files: { arg: string; name: string }[]) => void;   // publish the current folder's ordered files (for header ‹/›)
  filesFirst?: boolean;   // render files ABOVE subfolders (drums: keep the named grooves on top, genre folders below)
}) {
  const [path, setPath] = useState(root);                        // current REAL folder (absolute SD path)
  const [virt, setVirt] = useState<InjectFolder | null>(null);   // inside an injected virtual folder
  const [entries, setEntries] = useState<BrowseEntry[] | null>(null);   // null = loading
  const [err, setErr] = useState('');

  // Reset to the root when the root prop changes (component reused across cards).
  useEffect(() => { setPath(root); setVirt(null); }, [root]);

  // Publish the current folder's ordered playable files up to the deck, so the header ‹/› step within
  // the folder you're viewing (not the flat catalog). Recomputed whenever the visible list changes;
  // while loading (entries === null) we keep the previous list so a mid-fetch ‹/› still works.
  useEffect(() => {
    if (!onFolderList) return;
    if (virt) { onFolderList(virt.leaves.map(lf => ({ arg: lf.arg, name: lf.name }))); return; }
    if (entries) onFolderList(entries.filter(e => e.type === 'F').map(e => ({ arg: path + '/' + e.name, name: stripExt(e.name, ext) })));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [entries, virt, path, ext]);

  // Live @LS fetch whenever the real path changes (skip while inside a virtual folder). The
  // `alive` gate drops a stale reply when the user navigates away mid-fetch.
  useEffect(() => {
    if (!enabled || virt) { if (!enabled) setEntries(null); return; }
    let alive = true;
    setEntries(null); setErr('');
    tp.browse(path, ext)
      .then(r => { if (alive) setEntries(sortEntries(r.entries)); })
      .catch(e => { if (alive) { setEntries([]); setErr(String((e as any)?.message || e || 'browse failed')); } });
    return () => { alive = false; };
  }, [path, ext, enabled, virt, tp]);

  const atRoot = path === root && !virt;
  const goUp = () => {
    if (virt) { setVirt(null); return; }
    if (path !== root) setPath(path.split('/').slice(0, -1).join('/') || '/');
  };
  // Breadcrumb: the root label, each folder segment below it, then the virtual folder (if any).
  const rootName = root.split('/').pop() || root;
  const rel = path.startsWith(root) ? path.slice(root.length).replace(/^\//, '') : '';
  const segs = rel ? rel.split('/') : [];
  const crumbs: { label: string; go: () => void }[] = [{ label: rootName, go: () => { setVirt(null); setPath(root); } }];
  { let acc = root; for (const seg of segs) { acc += '/' + seg; const p = acc; crumbs.push({ label: seg, go: () => { setVirt(null); setPath(p); } }); } }
  if (virt) crumbs.push({ label: virt.name, go: () => {} });

  // One file/leaf row: mark `sel` when it's the chosen arg, prefix ♪ when it's playing.
  const fileRow = (rowArg: string, disp: string) => (
    <ListBtn key={rowArg} label={(playing === rowArg ? '♪ ' : '') + disp} sel={selected === rowArg}
      onPress={() => onSelectFile(rowArg, disp)} />
  );

  return (
    <>
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
      {!enabled ? <Text style={s.muted}>Connect to browse files.</Text> : virt ? (
        <ScrollView style={s.picker} nestedScrollEnabled>
          {virt.leaves.length === 0 ? <Text style={s.muted}>(empty)</Text>
            : virt.leaves.map(lf => fileRow(lf.arg, lf.name))}
        </ScrollView>
      ) : entries === null ? (
        <View style={{ padding: 20, alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={[s.muted, { marginTop: 8 }]}>Loading…</Text></View>
      ) : (
        <ScrollView style={s.picker} nestedScrollEnabled>
          {atRoot && (injectFolders || []).map(f => <ListBtn key={'@' + f.name} label={'📁 ' + f.name} onPress={() => setVirt(f)} />)}
          {(() => {
            const folders = entries.filter(e => e.type === 'D').map(e => <ListBtn key={'d/' + e.name} label={'📁 ' + e.name} onPress={() => setPath(path + '/' + e.name)} />);
            const files = entries.filter(e => e.type === 'F').map(e => fileRow(path + '/' + e.name, stripExt(e.name, ext)));
            return filesFirst ? [...files, ...folders] : [...folders, ...files];   // drums keep grooves on top
          })()}
          {!!err && <Text style={[s.muted, { padding: 12 }]}>⚠ {err}</Text>}
          {!err && entries.length === 0 && (!atRoot || !(injectFolders || []).length) && <Text style={s.muted}>(empty folder)</Text>}
        </ScrollView>
      )}
    </>
  );
}
