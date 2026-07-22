// App.tsx — T-DSP unified control surface (react-native-web on desktop, native in the app).
// One React/TS codebase over a platform-split transport (Web Serial / BLE). The on-device
// catalog DB (/tdsp/*.ndjson, built by @REINDEX) is the source of truth; browsing is local,
// only actions hit the wire. Old single-file UI preserved as App.old.tsx.
//
// Navigation: a lightweight state router. The homepage lists every section as a card
// (title + live value + the same header controls); tapping a card opens that section's
// own page. No nav library — just a `route` string ('home' | section id).
import React, { useState, useRef, useEffect, useMemo } from 'react';
import { View, Text, Pressable, ScrollView, FlatList, TextInput, Switch, StyleSheet, ActivityIndicator, Platform, Alert, useWindowDimensions, AppState as RNAppState } from 'react-native';
import type { AppStateStatus } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import Slider from '@react-native-community/slider';
import { createTransport } from './src/transportFactory';
import { createDiscovery } from './src/discoveryFactory';
import type { TdspDevice } from './src/discovery';
import { Catalog, CatalogCache, DrumFont, EMPTY_CATALOG, loadCatalog, LoadProgress, parseFonts, Song, songArg } from './src/catalog';
import type { Transport, DirPage, TransportKind } from './src/transport';
import { sortEntries } from './src/browse';
import type { BrowseEntry } from './src/browse';
import ArpStepGrid from './src/ui/ArpStepGrid';
import PianoRoll from './src/ui/PianoRoll';
import ArpPresetBrowser from './src/ui/ArpPresetBrowser';
import { ARP_PATTERNS as ARP_PAT, ARP_RATES, rateIndexFromFw, PAT_USER_SEQUENCE, DEFAULT_SHAPE, SeqStep, encodeSequence, encodeArpParams } from './src/arpSeq';
import { applyArpPreset, ArpPreset, ARP_LIBRARY } from './src/arpLibrary';

const EMPTY_DIR: DirPage = { path: '', page: 0, npages: 1, folders: [], carts: [] };

// Persistent catalog cache (AsyncStorage → localStorage on web). loadCatalog() short-circuits
// the whole NDJSON download when the device's index.ndjson is byte-identical to last time, so a
// reconnect (e.g. after the phone comes out of your pocket) is near-instant instead of a full
// re-download. Keyed per engine so switching boards doesn't cross-contaminate. See catalog.ts.
const CATALOG_KEY = 'tdsp.catalog.v1';
const catalogCache: CatalogCache = {
  get: () => AsyncStorage.getItem(CATALOG_KEY),
  set: (v: string) => AsyncStorage.setItem(CATALOG_KEY, v),
};
// Display name for a groove SD path (basename minus .mid) — the drum-track card's "value".
const grooveDisp = (p: string | null | undefined) => (p ? (p.split('/').pop() || '').replace(/\.mid$/i, '') : '');
const kb = (n: number) => (n / 1024).toFixed(1);   // bytes -> "12.3" KB, for the load progress readout

const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', accent2: '#a371f7', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };
// Per-section theme: a title/border accent + a translucent card background (over the dark app bg),
// so each area reads as its own color and a submenu's sub-cards inherit the parent's tint. Each is
// { accent, tint } — accent tints the title/left-border; tint is the see-through card fill.
const th = (accent: string, a: number) => ({ accent, tint: accent + Math.round(a * 255).toString(16).padStart(2, '0') });
const THEME = {
  synthA:   th('#3fb950', 0.14),   // green
  synthB:   th('#a371f7', 0.15),   // purple
  synthC:   th('#2dd4bf', 0.14),   // teal (voice 3, 4-voice pool)
  synthD:   th('#e3b341', 0.14),   // gold (voice 4)
  tempo:    th('#e3b341', 0.14),   // amber
  bt:       th('#58a6ff', 0.14),   // blue
  usb:      th('#79c0ff', 0.14),   // light blue (USB audio interface)
  settings: th('#ff7b72', 0.13),   // coral
  recorder:  th('#f85149', 0.14),  // red (MIDI record)
  audioloop: th('#f778ba', 0.14),  // pink (audio loop)
  drums:     th('#f0883e', 0.14),  // orange (drum track)
};
const HDR_H = 38;   // shared height for page-header control buttons (back / keyboard / transport) so they line up
// Friendly names for Transport.name (the wire values are 'USB' | 'BLE' | 'WIFI').
const TP_LABEL: Record<string, string> = { USB: 'USB', BLE: 'Bluetooth', WIFI: 'Wi-Fi' };
// What the 'default' transport actually is on this platform — Metro picks the factory
// (Web Serial on desktop, BLE on native), so the picker label has to match.
const DEFAULT_TP_LABEL = Platform.OS === 'web' ? 'USB' : 'Bluetooth';
// TAC5212 DAC high-pass filter presets (@HPF mode). 0 = off (all-pass); the rest are
// sub-audio cutoffs that block DC/rumble. Index === the firmware mode number.
const HPF_MODES = [
  { mode: 0, label: 'Off' },
  { mode: 1, label: '1 Hz' },
  { mode: 2, label: '12 Hz' },
  { mode: 3, label: '96 Hz' },
];
// The header VOL / TAC5212 output slider maps 1..100% → -60..0 dB on the DAC (0 = mute),
// mirroring the firmware's setMasterVolumePct. Shown as a dB readout in the codec section.
const volDb = (pct: number) => (pct <= 0 ? '-∞' : (-60 + 0.60 * pct).toFixed(1));

// What the MIDI player does when the current song finishes. The header button cycles
// through these; default 'stop'. Only 'repeat' uses the firmware's seamless loop
// (tp.songLoop); 'continue'/'shuffle' advance the song app-side when it ends (@SONGP=-1).
type EndMode = 'shuffle' | 'repeat' | 'continue' | 'stop';
const END_MODES: { key: EndMode; icon: string; label: string }[] = [
  { key: 'shuffle',  icon: '🔀', label: 'Shuffle'    },  // twisted arrows → random next song
  { key: 'repeat',   icon: '🔁', label: 'Repeat'     },  // spinning arrows → loop this song
  { key: 'continue', icon: '➡',  label: 'Continue'   },  // arrow → play the next song
  { key: 'stop',     icon: '◻',  label: 'Stop after' },  // hollow square → stop when it ends (default)
];

// Loop-recorder states, indexed by the firmware's 0..4 state code (@RECP / @STATE rec.st*).
// Module scope so both the per-player record rows and the standalone Loop Recorder card use one list.
const REC_STATES = ['Idle', 'Armed — play a note', 'Recording', 'Overdubbing', 'Looping'];

// The opaque app-owned state we persist on the device (@APP=) so a reload/reconnect restores
// it. Keep it small (device RAM buffer is fixed) and JSON-serializable; grow it as more
// firmware-invisible UI settings need to survive a reconnect.
type AppState = { end: EndMode; end2?: EndMode; groove?: string };   // end2 = MIDI Player 2's end-of-song mode; groove = last-selected drum groove path (the device only reports the kit, not the picked groove)
const isEndMode = (v: any): v is EndMode => END_MODES.some(m => m.key === v);

function notify(msg: string) { if (Platform.OS === 'web') (globalThis as any).alert?.(msg); else Alert.alert('T-DSP', msg); }

// The subtitle line under a section title: the live accent value, else a muted status tag.
const Subtitle = ({ value, status }: { value?: string; status?: string }) =>
  !!value ? <Text style={s.drawerValue} numberOfLines={1}>{value}</Text>
    : !!status ? <Text style={s.tag}>{status}</Text> : null;

// A slim 0..1 progress bar (song playback position). Sits right under the track title.
const ProgressBar = ({ value }: { value: number }) => (
  <View style={s.progTrack}><View style={[s.progFill, { width: `${Math.max(0, Math.min(1, value)) * 100}%` }]} /></View>
);

// Header beat lights: one circle per beat of the bar, the current beat lit, beat 1 (the
// downbeat / accent) in a distinct color. Two clock sources, preferring the device:
//   • LIVE (`live`): the device's @BEAT feed — locked to the REAL master downbeat + meter.
//   • LOCAL fallback: a self-correcting clock at the master BPM, for firmware that doesn't
//     emit @BEAT. Matches the tempo but re-anchors its downbeat to "now" on tempo/meter change.
// Only this component re-renders each beat (its own state), not the whole app.
const DOWNBEAT = '#e3b341';   // amber — the accented beat 1, distinct from the green beats
function BeatStrip({ sig, bpm, active, live }: { sig: number; bpm: number; active: boolean; live: { i: number; n: number } | null }) {
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
function Card({ title, value, status, subtitle, actions, foot, progress, onPress, onBack, style, accent, tint, topRight, disabledReason }:
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
function PageHeader({ title, value, status, subtitle, actions, progress, onBack, accent, tint, topRight }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; progress?: number; onBack: () => void; accent?: string; tint?: string; topRight?: React.ReactNode }) {
  const { width } = useWindowDimensions();
  const narrow = width < 640;
  return (
    <View style={[s.pageHead, tint && { backgroundColor: tint }, accent && { borderBottomColor: accent }]}>
      <View style={s.pageHeadRow}>
        <View style={s.drawerLeft}>
          <Text style={[s.pageTitle, accent && { color: accent }]}>{title}</Text>
          {subtitle ?? <Subtitle value={value} status={status} />}
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
class ProgressBus {
  private listeners = new Set<(p: LoadProgress | null) => void>();
  private last: LoadProgress | null = null;
  emit = (p: LoadProgress | null) => { this.last = p; this.listeners.forEach(l => l(p)); };
  get value() { return this.last; }
  subscribe(l: (p: LoadProgress | null) => void) { this.listeners.add(l); l(this.last); return () => { this.listeners.delete(l); }; }   // replay latest so a subscriber can't miss the mount→effect gap
}

// The "Loading catalog…" screen. Owns progress + elapsed-seconds state locally (fed by the
// ProgressBus) so its 10x/sec updates re-render this component alone, not the whole App.
function LoadScreen({ bus, tpLabel }: { bus: ProgressBus; tpLabel: string }) {
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
const HdrBtn = ({ label, onPress, stop, active }: { label: string; onPress: () => void; stop?: boolean; active?: boolean }) => (
  <Pressable onPress={onPress} style={[s.hdrBtn, stop && s.hdrBtnStop, active === false && s.hdrBtnIdle]}><Text style={s.hdrBtnText} numberOfLines={1}>{label}</Text></Pressable>
);
// A small keyboard glyph drawn with Views so it can be tinted (an emoji can't): a bordered
// body with five keys. WHITE = this synth owns the USB keyboard; GREY = another synth does.
function KbdGlyph({ color }: { color: string }) {
  return (
    <View style={{ width: 26, height: 17, borderWidth: 1.5, borderColor: color, borderRadius: 3, paddingHorizontal: 2.5, paddingBottom: 2.5, flexDirection: 'row', alignItems: 'flex-end', justifyContent: 'space-between' }}>
      {[0, 1, 2, 3, 4].map(i => <View key={i} style={{ width: 2.5, height: 7, backgroundColor: color, borderRadius: 1 }} />)}
    </View>
  );
}
// Keyboard-ownership control: tap a GREY keyboard to route the USB keyboard to this synth.
const KbdBtn = ({ owned, onPress }: { owned: boolean; onPress: () => void }) => (
  <Pressable onPress={onPress} hitSlop={10} style={s.kbdBtn}
    accessibilityLabel={owned ? 'USB keyboard plays this synth' : 'Tap to play this synth with the USB keyboard'}>
    <KbdGlyph color={owned ? C.text : C.muted} />
  </Pressable>
);
const Row = ({ children }: any) => <View style={s.row}>{children}</View>;
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
const ThrottledSlider = ({ value, onChange, onCommit, min = 0, max = 150, step = 1, disabled, style }:
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
const VolSlider = ({ label, value, onChange, onCommit, disabled, max = 150 }:
  { label: string; value: number; onChange: (v: number) => void; onCommit: (v: number) => void; disabled?: boolean; max?: number }) => (
  <View style={s.volRow}>
    <Text style={[s.muted, { width: 52 }]}>{label}</Text>
    <ThrottledSlider value={value} onChange={onChange} onCommit={onCommit} max={max} disabled={disabled} />
    <Text style={[s.muted, { width: 44, textAlign: 'right' }]}>{Math.round(value)}%</Text>
  </View>
);
const Stat = ({ label, n, sub }: { label: string; n: number; sub?: string }) => (
  <View style={s.stat}><Text style={s.statN}>{n}</Text><Text style={s.statL}>{label}</Text>{!!sub && <Text style={s.statSub}>{sub}</Text>}</View>
);
const ListBtn = ({ label, sel, onPress }: any) => (
  <Pressable onPress={onPress} style={[s.listBtn, sel && s.listBtnSel]}><Text style={s.text} numberOfLines={1}>{label}</Text></Pressable>
);
// A submenu page: full-width cards (one per row) for a parent section's children (e.g. Settings →
// Connection, TAC5212). Tapping a card opens that child's own existing page via onOpen(id).
// `getItems` is a getter so it can read the sections array lazily (it isn't assigned yet when the
// body is built).
// `accent`/`tint`, when given, override each child's own colors so ALL sub-cards match the parent
// tile — a quick visual cue for which section (e.g. which synthesizer) you're inside.
// A tab strip INSIDE a page body (e.g. a MIDI player's Player / Looper split). This is not the
// old page-level tab nav — the router still uses menu > submenu; these just split one card's own
// content. Tab state is local, so switching never touches the route. Reuses the arp's tab styling.
function BodyTabs({ tabs }: { tabs: { key: string; label: string; body: React.ReactNode }[] }) {
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
function SubMenu({ getItems, onOpen, accent, tint }: { getItems: () => any[]; onOpen: (id: string) => void; accent?: string; tint?: string }) {
  return (
    <View style={s.submenu}>
      {getItems().map(sec => (
        <Card key={sec.id} title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} actions={sec.actions} foot={sec.foot}
          onPress={() => onOpen(sec.id)} style={s.cardGrid} accent={accent ?? sec.accent} tint={tint ?? sec.tint} topRight={sec.topRight} disabledReason={sec.disabledReason} />
      ))}
    </View>
  );
}
const ROW_H = 41;   // fixed list-row height so FlatList.scrollToIndex is reliable
type VItem = { key: string; label: string; i: number };

// ---- <FolderBrowser> : the generic recursive SD file/folder browser (the @LS client) -------
// One reusable component behind the MIDI-player and Drums file pickers (and, later, any SD tree
// — samples/soundfonts). Drills folder-by-folder from `root` via transport.browse() (@LS), lists
// files filtered by `ext` (sorted client-side by ./browse.sortEntries), and calls onSelectFile
// with the FULL SD path + a display name. Mirrors the Voices browser's breadcrumb + up-button UX
// and reuses ListBtn. `injectFolders` adds synthetic folders at the ROOT level whose leaves play
// by their own arg (used for the baked "tests" songs, which live in flash, not on the card).
// `enabled` gates the live fetch (connected + catalog loaded). `selected`/`playing` are the
// currently-chosen / currently-playing arg (full path or a baked name) for row highlighting.
type InjectFolder = { name: string; leaves: { name: string; arg: string }[] };
const stripExt = (name: string, ext?: string) => {
  const suf = ext ? '.' + ext : '.mid';
  return name.toLowerCase().endsWith(suf.toLowerCase()) ? name.slice(0, -suf.length) : name;
};
function FolderBrowser({ tp, root, ext, enabled, selected, playing, onSelectFile, injectFolders, onFolderList, filesFirst }: {
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

export default function App() {
  // Transport selection (session-only — the app has no local persistence, so this resets
  // on reload). 'default' = the platform's built-in (Web Serial on desktop, BLE on native);
  // 'wifi' = LAN WebSocket to the ESP32 (firmware built with -D TDSP_CTRL_WIFI).
  const [tkind, setTkind] = useState<TransportKind>('default');
  const [wifiHost, setWifiHost] = useState('');   // '' = the firmware's mDNS name (tdsp.local)
  // The built-in transport is cached and reused: BleTransport owns a native BleManager, so
  // rebuilding it on every toggle would leak. The WiFi one is just a URL holder (it opens
  // nothing until connect()), so it's cheap to rebuild as the host box is typed into —
  // which is exactly what keeps `tp` in sync with what's on screen, with no commit/blur
  // dance before tapping Connect. The picker is disabled unless disconnected+idle, so this
  // can never swap a live link out from under us.
  const defTpRef = useRef<Transport | null>(null);
  const tp = useMemo<Transport>(() => {
    if (tkind === 'wifi') return createTransport('wifi', wifiHost.trim() || undefined);
    if (!defTpRef.current) defTpRef.current = createTransport('default');
    return defTpRef.current;
  }, [tkind, wifiHost]);

  // mDNS discovery state. The browse effect lives further down — it depends on
  // connected/connecting, which are declared below (referencing them here would be a TDZ
  // error, since const bindings aren't usable before their declaration).
  const discoRef = useRef(createDiscovery());
  const [found, setFound] = useState<TdspDevice[]>([]);
  const [scanning, setScanning] = useState(false);
  const { width } = useWindowDimensions();
  const cols = width < 560 ? 1 : width < 900 ? 2 : 3;   // responsive homepage grid columns
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [userDisc, setUserDisc] = useState(false);      // user tapped Disconnect App → suppress auto-reconnect
  const userDiscRef = useRef(false);                    // synchronous mirror of userDisc so an in-flight connect() can see a cancel immediately
  const connectingRef = useRef(false);                  // synchronous guard so the auto-poll can't double-connect

  // mDNS discovery: browse _tdsp._tcp while the Wi-Fi picker is open and disconnected, so
  // you tap a device instead of hunting for its IP. Each hit carries its RESOLVED address,
  // so connecting never depends on the platform resolving a .local name — and several
  // T-DSPs on one LAN all show up. Web has no mDNS (supported === false) → host box only.
  useEffect(() => {
    const d = discoRef.current;
    // Only scan when it's actually usable: Wi-Fi selected, disconnected, not mid-connect.
    if (!d.supported || tkind !== 'wifi' || connected || connecting) { d.stop(); setScanning(false); return; }
    setScanning(true); setFound([]);
    d.start(setFound);
    return () => { d.stop(); setScanning(false); };
  }, [tkind, connected, connecting]);
  useEffect(() => () => discoRef.current.stop(), []);   // release the scanner on unmount
  // Catalog-load progress rides a ProgressBus (module scope) into <LoadScreen>, NOT App state,
  // so the ~10/sec load ticks don't re-render the whole App (that starved the USB reader and
  // stalled the transfer). The bus is stable for the App's lifetime.
  const busRef = useRef<ProgressBus | null>(null);
  const progBus = (busRef.current ??= new ProgressBus());
  const manualStopRef = useRef(false);                  // set on user Stop so the resulting @SONGP=-1 isn't treated as a natural song end
  const drumStopRef = useRef(false);                    // drum-deck manual-stop flag (drums have no position feed, so inert — kept for the shared deck API)
  const onSongEndRef = useRef<() => void>(() => {});     // latest "song finished naturally" handler (continue/shuffle); kept in a ref so the @SONGP listener never goes stale
  const manualStop2Ref = useRef(false);                 // same guards for MIDI Player 2 (@SONG2P feed)
  const onSong2EndRef = useRef<() => void>(() => {});
  // App-owned settings the firmware can't derive but must echo back so an app reload/reconnect
  // restores them (persisted opaque on the device via @APP=, see saveAppState). This ref is the
  // single source of what we persist — add a field here + hydrate it below to save more.
  const appStateRef = useRef<AppState>({ end: 'stop' });
  const [cat, setCat] = useState<Catalog>(EMPTY_CATALOG);
  const [loaded, setLoaded] = useState(false);
  const [route, setRoute] = useState<string>('home');   // 'home' or a section id
  const [vol, setVol] = useState(80);
  const [hpf, setHpf] = useState(0);                    // TAC5212 DAC high-pass filter mode (0=off)
  const lastHpfRef = useRef(2);                         // remembers the last non-off cutoff so the Enable switch can restore it
  const [bt, setBt] = useState({ conn: false, peer: '' });
  const [arp, setArp] = useState({ on: false, pat: 0, rate: 0, oct: 1, latch: false });
  const [seq, setSeq] = useState<SeqStep[]>(() => DEFAULT_SHAPE.steps.map(s => ({ ...s })));   // User Sequence step table (device doesn't echo it, so the app owns it)
  const [arpPresetId, setArpPresetId] = useState<string>('');   // which library preset is applied (for browser highlight)
  const [arpMode, setArpMode] = useState<'preset' | 'manual'>('preset');   // which editor drives the arp — one at a time so they never collide
  const [drums, setDrums] = useState<{ kit: number; sel: string | null; playing: string | null }>({ kit: 0, sel: null, playing: null });
  const [drumVol, setDrumVol] = useState(100);        // drum-player level 0..150 %, independent of the master @VOL
  // `song` = the selected song's NAME (its stable identity now that there's no index). name = currently-playing title.
  const [player, setPlayer] = useState<{ song: string; playing: boolean; name: string; prog: number }>({ song: '', playing: false, name: '', prog: 0 });
  const [songVol, setSongVol] = useState(100);        // MIDI-player level 0..150 %, independent of the master @VOL
  const [endMode, setEndMode] = useState<EndMode>('stop');   // what the player does when a song ends
  // MIDI Player 2 (voice-2 song player, caps.voice2): a second, independent song player so two
  // songs play at once. Its own selection/playback/end-mode; its output rides the voice-2 bus
  // (voice2.vol / @VOICE2VOL), so there's no separate volume slider here.
  const [player2, setPlayer2] = useState<{ song: string; playing: boolean; name: string; prog: number }>({ song: '', playing: false, name: '', prog: 0 });
  const [endMode2, setEndMode2] = useState<EndMode>('stop');
  const [bpm, setBpm] = useState(120);
  const [beatFeed, setBeatFeed] = useState<{ i: number; n: number } | null>(null);   // live @BEAT from the device (null = fall back to the local clock)
  const beatStaleRef = useRef<any>(null);
  const [quant, setQuant] = useState(false);           // launch quantize: start songs/grooves on the next bar
  // The metronome IS the master transport: `on` = the clock/transport is RUNNING (everything locks
  // to it); `muted` = whether the click is audible (default muted — the transport runs silently).
  // cap=true once the device reports metronome support; sig = beats/bar; vol = click level 0..150 %.
  // locked = tempo lock: when true the device holds the master BPM (loading a song/groove no longer
  // auto-sets it); when false the sole piece of content to start sets the tempo. Default off.
  const [metro, setMetro] = useState({ on: false, muted: true, cap: false, sig: 4, vol: 100, locked: false });
  const [songBpm, setSongBpm] = useState(120);            // tempo of the last song that played
  const [selVoice, setSelVoice] = useState('');
  const [selVoiceName, setSelVoiceName] = useState('');   // last-picked instrument name (shown on the card, persists across browsing)
  const [selVoicePath, setSelVoicePath] = useState('');   // full location of the picked instrument (e.g. /dexed/<cart>.syx, or "Bundled") — shown under the name
  // Voices 2 (build-flag gated): a second Dexed voice on the keyboard half of the pool.
  // `caps` = which optional features the connected firmware was built with (from @STATE),
  // so the Voices 2 / Arpeggiator 2 cards SHOW only when compiled in. Each has its own
  // selection + volume + arp state; the folder browser is shared (pickVoice takes a target).
  // caps.audioloop is a COUNT (how many audio loops the device actually allocated —
  // RAM/PSRAM dependent), not a bool: 0 hides the Audio Loop card entirely.
  const [caps, setCaps] = useState({ voice2: false, arp2: false, rec: false, recedit: false, audioloop: 0, fx: false, usbaudio: false, drumkitsel: true, drumfontsel: false });
  // Runtime drum-font swap (build-flag gated, caps.drumfontsel → the sampled-drum TSF build with >1 SF2
  // on the card). `fonts` = the swappable list from the device's "@FONTS=" line; `drumFont` = the
  // resident font (path + display) from @STATE.drumfont. Selecting one sends @DRUMFONT= (a picker, not
  // a layer); the device acks with a "@DRUMFONT=" line, which triggers a catalog reload for the new kits.
  const [fonts, setFonts] = useState<DrumFont[]>([]);
  const [drumFont, setDrumFont] = useState<{ path: string; display: string }>({ path: '', display: '' });
  // USB Audio interface (build-flag gated, caps.usbaudio → TDSP_USB_AUDIO). The device is a
  // 24-bit/48k USB sound card: host audio in → mix bus, the mix out → host. `active` = the host
  // has the audio stream open (playing/recording); `gain` = the USB-in return level (0..150 %,
  // @USBGAIN); rxover/txunder = ring-buffer over/underrun counters (drift diagnostics, from
  // usbIn.getStatus()). See planning/usb-audio-mixkit/DESIGN.md.
  const [usbAudio, setUsbAudio] = useState({ active: false, gain: 100, rxover: 0, txunder: 0 });
  // FX reverb master insert (build-flag gated, caps.fx). One reverb per build — @STATE.fx.type is
  // 'plate' (rich: size/damp/diff/shimmer/freeze) or 'spring' (time/treble/bass); both share mix.
  // The card renders the type-appropriate sliders. Off = clean bypass (the build sounds identical).
  const [fx, setFx] = useState({ type: 'plate' as 'plate' | 'spring', on: false, mix: 30,
    size: 70, damp: 40, lodamp: 10, diff: 65, lowpass: 100, hipass: 0, shimmer: 0, pitch: 0, freeze: false,
    time: 55, treble: 40, bass: 30,
    // SEND-mode extras (firmware @STATE.fx.route === 'send'): the reverb is a per-voice aux SEND bus
    // rather than a master insert. `return` = wet return level (the old "mix"); `loopsend` = audio-loop
    // send; per-synth/drum sends live in `trkSend` (from @STATE tracks[].fxsend). route stays 'insert'
    // on insert-only builds so the card renders the classic master reverb.
    route: 'insert' as 'insert' | 'send', return: 60, loopsend: 0 });
  // Per-track reverb SEND levels (0..100) keyed by track index, hydrated from @STATE tracks[].fxsend.
  const [trkSend, setTrkSend] = useState<Record<number, number>>({});
  // Features compiled in but unavailable right now, keyed by card id -> reason (e.g. "PSRAM required").
  // The firmware reports these in @STATE.unavail; a card whose id is present renders greyed with the
  // reason instead of hiding. psram = MB of soldered PSRAM (0 on the no-PSRAM boards).
  const [unavail, setUnavail] = useState<Record<string, string>>({});
  const [psram, setPsram] = useState(0);
  const [voice2, setVoice2] = useState({ on: false, vol: 100, name: '', path: '' });
  // Runtime pool partition (4-voice pool builds, @STATE.pool): the 8 Dexed engines redistributed among
  // the 4 fixed voices. `preset` 0=4voices 1=2voices 2=1voice 3=4+2+2; `engines[v]` = engines that voice
  // owns (0 = absorbed/idle); `active[v]` = voice is live. `has` gates the picker card to pool builds.
  const [pool, setPool] = useState({ has: false, preset: 0, engines: [2, 2, 2, 2], active: [true, true, true, true] });
  // Per-track live-MIDI subscription (Phase 3, Thread C), keyed by firmware track index i. `src` =
  // which input device feeds that synth ("none"/"din"/"usb"/"multi"/…), `srcch` = channel filter
  // (0 = all). Hydrated from @STATE tracks[]; the MIDI-Input selector on each synth card writes it
  // via @TRK<i>.SRC / SRCCH. Data-driven: a new voice's entry just appears here, no per-voice field.
  const [trkSubs, setTrkSubs] = useState<Record<number, { src: string; srcch: number }>>({});
  // The drum track's live-MIDI index (= synthCount) when the firmware supports live finger-drumming
  // (drum @STATE entry carries "src"); null on older firmware / no drum engine, so the drums card
  // hides its keyboard button. Kept separate from synthCount so claimUsb can include it exclusively.
  const [drumSrcIdx, setDrumSrcIdx] = useState<number | null>(null);
  // The running firmware has a live, drum-capable track (from @STATE tracks[] kind:"drum", on:1).
  // This is GROUND TRUTH for "this build renders drums" — independent of the cached SD catalog,
  // whose meta.drums can lag (the on-device catalog auto-heal keys on the synth NAME, so toggling
  // a parallel drum voice on the same synth doesn't rebuild it). We OR it into the Drums card's
  // visibility so a working drum engine is never hidden by a stale catalog; a Refresh reloads the
  // grooves/kits. See project_drum_grooves / the OPLL DrumVoice build.
  const [drumLive, setDrumLive] = useState(false);
  // Data-driven synth voices (Phase 3): how many synth cards to render + each voice's name/level,
  // all from @STATE tracks[]. Voices 0/1 keep their bespoke cards (Synth A/B); voices 2+ get a
  // generated card driven by @TRK<i>.* (selVoiceX = the browser selection per extra voice index).
  const [synthCount, setSynthCount] = useState(1);
  const [trkNames, setTrkNames] = useState<Record<number, string>>({});
  const [trkEng, setTrkEng] = useState<Record<number, string>>({});   // per-track engine tag from @STATE tracks[].eng (e.g. 'opll'); Dexed tracks carry none
  const [opllIdx, setOpllIdx] = useState<Record<number, number>>({});   // per-track CURRENT instrument index for NON-Dexed engines (synced from the @TRK<i>.INSTR= echo); they step @TRK<i>.INSTR=, not the /dexed catalog
  const [trkNinstr, setTrkNinstr] = useState<Record<number, number>>({});     // per-track engine instrument COUNT (from @STATE tracks[].ninstr) — lets ‹/› wrap for any engine
  const [trkEngInstrs, setTrkEngInstrs] = useState<Record<number, string[]>>({});   // per-track engine PATCH LIST (from @TRK<i>.INSTRS) — the non-Dexed engines' own voice picker
  const [trkSong, setTrkSong] = useState<Record<number, string>>({});   // per-track LOADED MIDI song from @STATE tracks[].song — hydrates on connect (esp. C/D, whose live playerX isn't in @STATE)
  const [selVoiceX, setSelVoiceX] = useState<Record<number, string>>({});   // browser sel key, keyed by 0-based voice index (>=2)
  const [trkVolX, setTrkVolX] = useState<Record<number, number>>({});       // per-extra-voice level (@TRK<i>.VOL), local echo
  // Per-extra-voice (index >=2) MIDI-player + arp state, so Synth C/D get the SAME submenu as A/B
  // (Synth/Voices + MIDI Player + Arpeggiator). Map-keyed by 0-based voice index; the deck/arp
  // slots are built from these in a loop and wired to @TRK<i>.{PLAY,RESTART,STOP,LOOP,ARP*}.
  const [playerX, setPlayerX] = useState<Record<number, { song: string; playing: boolean; name: string; prog: number }>>({});
  const [endModeX, setEndModeX] = useState<Record<number, EndMode>>({});
  const [arpX, setArpX] = useState<Record<number, { on: boolean; pat: number; rate: number; oct: number; latch: boolean }>>({});
  const [arpModeX, setArpModeX] = useState<Record<number, 'preset' | 'manual'>>({});
  const [arpPresetIdX, setArpPresetIdX] = useState<Record<number, string>>({});
  const [seqX, setSeqX] = useState<Record<number, SeqStep[]>>({});
  const manualStopRefX = useRef<Record<number, React.MutableRefObject<boolean>>>({});
  // MIDI loop recorder (build-flag gated, caps.rec). Each synth's MIDI player owns its own
  // recorder, so every setting is PER-VOICE: bars1/bars2 = that synth's loop length, st1/st2 =
  // state (0 idle 1 armed 2 recording 3 overdub 4 playing), p1/p2 = record-fill / playback-phase.
  // `v` is only which voice the firmware's @REC* commands currently target — each player aims it
  // at its own voice before acting. Time signature is deliberately NOT here: it's the shared
  // master meter (metro.sig, set on the Metronome card) that both synths lock to.
  const [rec, setRec] = useState({ v: 1, bars1: 4, bars2: 4, st1: 0, st2: 0, p1: 0, p2: 0, max: 1024 });
  // Audio loop recorder (shown on caps.audioloop > 0): sel = selected loop, bars/mono/follow
  // = the selected loop's config, st[]/p[] = per-loop state (same 0..4 codes as `rec`) and
  // 0..1 progress, capS = the selected loop's capacity in seconds (bars that don't fit are
  // disabled). See planning/audio-looper/DESIGN.md.
  const [aloop, setAloop] = useState({ sel: 0, bars: 4, mono: false, follow: true, capS: 0, level: 100,
                                       st: [0, 0, 0], p: [0, 0, 0] });
  const [selVoice2, setSelVoice2] = useState('');   // voice-2 browser highlight (independent of voice 1)
  const [arp2, setArp2] = useState({ on: false, pat: 0, rate: 0, oct: 1, latch: false });
  const [seq2, setSeq2] = useState<SeqStep[]>(() => DEFAULT_SHAPE.steps.map(s => ({ ...s })));   // arp-2 User Sequence table (app-owned, mirrors seq)
  const [arp2PresetId, setArp2PresetId] = useState<string>('');
  const [arp2Mode, setArp2Mode] = useState<'preset' | 'manual'>('preset');
  const [cart, setCart] = useState<{ rel: string; name: string } | null>(null);
  const [vpath, setVpath] = useState('');                 // dexed folder-browser current path ('' = root)
  const [level, setLevel] = useState<DirPage>(EMPTY_DIR); // current /dexed folder listing (lazy @DXLS)
  const [cartVoices, setCartVoices] = useState<string[]>([]); // open cart's 32 voice names (lazy @DXVL)
  const [libBusy, setLibBusy] = useState(false);          // a browse/voices fetch is in flight
  const [libErr, setLibErr] = useState('');               // last /dexed browse error (shown in-UI for diagnosis)
  const [q, setQ] = useState({ voice: '', cart: '', groove: '' });
  const [busy, setBusy] = useState(false);

  // Hydrate every card from the device's real current settings (the @STATE reply). The
  // device only knows what's ACTIVE, so "selected but not playing" song/groove rows stay
  // as-is; everything the firmware tracks (vol/bpm/arp/loop/voice/kit/what's playing) is
  // restored. Sets state only — no echo back to the device.
  const clampIdx = (v: any, n: number) => Math.max(0, Math.min(n - 1, (v | 0)));
  // @STATE reports only a song's DISPLAY name; song identity is the play arg (full SD path).
  // Resolve the arg from the catalog by name so ‹ › / Play round-trip correctly after a reconnect;
  // fall back to the name itself if the catalog isn't loaded yet (best effort — see tracks note).
  const songArgByName = (nm: string) => { const r = cat.songs.find(s => s.name === nm); return r ? songArg(r) : nm; };
  function hydrate(j: any) {
    if (j.vol != null) setVol(j.vol);
    if (j.hpf != null) { const m = clampIdx(j.hpf, HPF_MODES.length); setHpf(m); if (m) lastHpfRef.current = m; }
    if (j.bpm != null) setBpm(j.bpm);
    if (j.metrolock != null) setMetro(m => ({ ...m, locked: !!j.metrolock }));   // tempo lock (content stops auto-setting the BPM)
    // Fallback end-mode guess from the loop flag, for firmware without @APP: loop on ⇒ Repeat;
    // loop off ⇒ keep the app's mode unless it was Repeat (then fall back to Stop). When @APP is
    // present its stored value arrives right after and overrides this (see hydrateApp).
    if (j.loop != null) setEndMode(m => j.loop ? 'repeat' : (m === 'repeat' ? 'stop' : m));
    if (j.quant != null) setQuant(!!j.quant);
    if (j.metro != null) setMetro(m => ({ ...m, on: !!j.metro, cap: true }));   // on = transport running (metronome-capable builds)
    if (j.metromuted != null) setMetro(m => ({ ...m, muted: !!j.metromuted }));   // is the click audible?
    if (j.metrosig != null) setMetro(m => ({ ...m, sig: Math.max(1, Math.min(16, j.metrosig | 0)) || 4 }));
    if (j.metrovol != null) setMetro(m => ({ ...m, vol: Math.max(0, Math.min(150, j.metrovol | 0)) }));
    if (j.arp) setArp({ on: !!j.arp.on, pat: clampIdx(j.arp.pat, ARP_PAT.length), rate: rateIndexFromFw(j.arp.rate | 0), oct: Math.max(1, Math.min(4, j.arp.oct | 0)) || 1, latch: !!j.arp.latch });
    if (j.song) setPlayer(p => ({ ...p, playing: !!j.song.playing, song: j.song.name ? songArgByName(j.song.name) : p.song, name: j.song.name || p.name, prog: j.song.p != null ? j.song.p / 1000 : (j.song.playing ? -1 : 0) }));
    if (j.song?.vol != null) setSongVol(Math.max(0, Math.min(150, j.song.vol | 0)));
    if (j.drums) setDrums(d => ({ ...d, kit: j.drums.kit | 0, playing: j.drums.playing ? d.playing : null }));
    if (j.drums?.vol != null) setDrumVol(Math.max(0, Math.min(150, j.drums.vol | 0)));
    // Resident drum font (runtime swap builds only emit j.drumfont): path + short display label.
    if (j.drumfont) setDrumFont({ path: j.drumfont.path || '', display: j.drumfont.display || '' });
    if (j.voice) {
      if (j.voice.cart) {
        const rel = j.voice.cart;
        setSelVoice('c' + rel + ':' + (j.voice.cv | 0)); setSelVoiceName(j.voice.name || ''); setSelVoicePath('/dexed/' + rel);
        // Preload the folder browser onto the current cart so Next/Prev step through its voices
        // immediately at startup — otherwise voiceData is empty and stepVoice() bails on cart voices
        // until the user manually dives into the folder/cart.
        const slash = rel.lastIndexOf('/');
        setVpath(slash >= 0 ? rel.slice(0, slash) : '');
        setCart({ rel, name: (slash >= 0 ? rel.slice(slash + 1) : rel).replace(/\.syx$/i, '') });
      }
      else if (j.voice.i != null && j.voice.i < 320) { setSelVoice('b' + (j.voice.i | 0)); setSelVoiceName(j.voice.name || ''); setSelVoicePath('Bundled'); }
    }
    // Voices 2 / Arp 2 — build capabilities (SHOW the cards) + the keyboard half's state.
    if (j.caps) setCaps({ voice2: !!j.caps.voice2, arp2: !!j.caps.arp2, rec: !!j.caps.rec,
                          recedit: !!j.caps.recedit, audioloop: Math.max(0, j.caps.audioloop | 0), fx: !!j.caps.fx,
                          usbaudio: !!j.caps.usbaudio,
                          // absent (older firmware) -> assume selectable; explicit 0 = a fixed drum voice (OPLL) with no GM kits
                          drumkitsel: j.caps.drumkitsel !== 0,
                          // runtime drum-font swap: only the sampled-drum TSF build with >1 SF2 on the card sets this
                          drumfontsel: !!j.caps.drumfontsel });
    // USB Audio interface status (usbaudio builds only emit j.usb): host-stream active flag,
    // USB-in return level, and the over/underrun counters used to spot host↔codec clock drift.
    if (j.usb) setUsbAudio(u => ({
      active: !!j.usb.active,
      gain: j.usb.gain != null ? Math.max(0, Math.min(150, j.usb.gain | 0)) : u.gain,
      rxover: j.usb.rxover != null ? (j.usb.rxover | 0) : u.rxover,
      txunder: j.usb.txunder != null ? (j.usb.txunder | 0) : u.txunder,
    }));
    // FX reverb state (type + params); firmware emits on/freeze as 0|1 -> coerce to bool.
    if (j.fx) setFx(f => ({ ...f, ...j.fx, on: !!j.fx.on, freeze: !!j.fx.freeze }));
    // Grey-out reasons for built-but-unavailable features + PSRAM presence (both @STATE top-level).
    if (j.unavail !== undefined) setUnavail(j.unavail || {});
    if (j.psram !== undefined) setPsram(j.psram | 0);
    // Runtime pool partition (only 4-voice pool builds emit j.pool).
    if (j.pool) setPool({
      has: true,
      preset: j.pool.preset | 0,
      engines: Array.isArray(j.pool.engines) ? j.pool.engines.map((x: number) => x | 0) : [2, 2, 2, 2],
      active: Array.isArray(j.pool.active) ? j.pool.active.map((x: number) => !!x) : [true, true, true, true],
    });
    if (j.aloop) setAloop(a => ({
      ...a,
      sel: Math.max(0, j.aloop.sel | 0),
      bars: [1, 2, 4, 8].includes(j.aloop.bars | 0) ? (j.aloop.bars | 0) : a.bars,
      mono: !!j.aloop.mono,
      follow: !!j.aloop.follow,
      capS: (j.aloop.cap | 0) / 10,                       // device reports tenths of a second
      st: a.st.map((v, i) => (i === (j.aloop.sel | 0) ? Math.max(0, Math.min(4, j.aloop.st | 0)) : v)),
      p: a.p.map((v, i) => (i === (j.aloop.sel | 0) ? (j.aloop.p | 0) / 1000 : v)),
    }));
    if (j.rec) setRec(r => ({
      v: j.rec.v === 2 ? 2 : 1,
      bars1: [1, 2, 4, 8].includes(j.rec.bars1 | 0) ? (j.rec.bars1 | 0) : r.bars1,
      bars2: [1, 2, 4, 8].includes(j.rec.bars2 | 0) ? (j.rec.bars2 | 0) : r.bars2,
      st1: Math.max(0, Math.min(4, j.rec.st1 | 0)),
      st2: Math.max(0, Math.min(4, j.rec.st2 | 0)),
      p1: (j.rec.p1 | 0) / 1000,
      p2: (j.rec.p2 | 0) / 1000,
      max: (j.rec.max | 0) || r.max,
    }));
    if (j.voice2) setVoice2(v => ({
      on: !!j.voice2.on,
      vol: j.voice2.vol != null ? Math.max(0, Math.min(150, j.voice2.vol | 0)) : v.vol,
      name: j.voice2.name || v.name,
      path: j.voice2.cart ? '/dexed/' + j.voice2.cart : (j.voice2.i != null ? 'Bundled' : v.path),
    }));
    if (j.arp2) setArp2({ on: !!j.arp2.on, pat: clampIdx(j.arp2.pat, ARP_PAT.length), rate: rateIndexFromFw(j.arp2.rate | 0), oct: Math.max(1, Math.min(4, j.arp2.oct | 0)) || 1, latch: !!j.arp2.latch });
    // MIDI Player 2 (voice-2 song player): restore what's playing + its loop flag → end-mode guess.
    if (j.song2) setPlayer2(p => ({ ...p, playing: !!j.song2.playing, song: j.song2.name ? songArgByName(j.song2.name) : p.song, name: j.song2.name || p.name, prog: j.song2.p != null ? j.song2.p / 1000 : (j.song2.playing ? -1 : 0) }));
    if (j.song2?.loop != null) setEndMode2(m => j.song2.loop ? 'repeat' : (m === 'repeat' ? 'stop' : m));
    // Per-track live-MIDI subscription (Thread C): each synth entry in tracks[] carries its input
    // device + channel filter. Rebuild the whole map so a removed/added track is reflected exactly.
    if (Array.isArray(j.tracks)) {
      const next: Record<number, { src: string; srcch: number }> = {};
      const names: Record<number, string> = {};
      const engs: Record<number, string> = {};
      const songs: Record<number, string> = {};
      const sends: Record<number, number> = {};   // per-track reverb SEND level (send-matrix builds)
      const ninstrs: Record<number, number> = {};   // per-track engine instrument count (for ‹/› wrap)
      let nSynth = 0; let dIdx: number | null = null; let dLive = false;
      for (const t of j.tracks) if (t && t.i != null) {
        if (typeof t.fxsend === 'number') sends[t.i | 0] = Math.max(0, Math.min(100, t.fxsend | 0));   // reverb send (synths + drums)
        if (typeof t.ninstr === 'number') ninstrs[t.i | 0] = t.ninstr | 0;
        if (t.kind === 'synth') {
          next[t.i | 0] = { src: t.src || 'none', srcch: t.srcch | 0 };
          if (typeof t.name === 'string') names[t.i | 0] = t.name;
          if (typeof t.eng === 'string') engs[t.i | 0] = t.eng;   // 'opll' etc; absent = Dexed
          if (typeof t.song === 'string') songs[t.i | 0] = t.song;   // loaded MIDI song (hydrates all cards, incl. C/D)
          nSynth++;
        } else if (t.kind === 'drum') {
          if (t.on) dLive = true;                            // a live, drum-capable track exists (ground truth)
          if (t.src != null) {                               // firmware with live finger-drumming (drum router)
            next[t.i | 0] = { src: t.src || 'none', srcch: t.srcch | 0 };
            dIdx = t.i | 0;
          }
        }
      }
      setTrkSubs(next);
      setTrkNames(names);
      setTrkEng(engs);
      setTrkSong(songs);
      setTrkSend(sends);
      setTrkNinstr(ninstrs);
      setDrumSrcIdx(dIdx);
      setDrumLive(dLive);
      if (nSynth > 0) setSynthCount(nSynth);   // how many synth cards to render (data-driven)
    }
  }

  // Restore the opaque app-owned state (@APP=). This is the authoritative source for settings
  // the firmware can't derive; it overrides hydrate()'s loop-based end-mode guess and keeps the
  // firmware loop flag in step. Each persisted field is validated + applied here — add new ones
  // alongside `end` and mirror them in appStateRef so persistApp() keeps sending the full blob.
  function hydrateApp(a: any) {
    if (!a || typeof a !== 'object') return;
    if (isEndMode(a.end)) { appStateRef.current.end = a.end; setEndMode(a.end); tp.songLoop(a.end === 'repeat'); }
    if (isEndMode(a.end2)) { appStateRef.current.end2 = a.end2; setEndMode2(a.end2); tp.song2Loop(a.end2 === 'repeat'); }
    // The device's @STATE reports the drum kit but not which groove is picked, so restore the
    // last-selected groove here — that way the Drums card shows it and ‹/› step from it at startup.
    if (typeof a.groove === 'string' && a.groove) { appStateRef.current.groove = a.groove; setDrums(d => ({ ...d, sel: a.groove })); }
  }

  useEffect(() => tp.onLine(line => {
    if (line.startsWith('@STATE=')) {
      try { hydrate(JSON.parse(line.slice(line.indexOf('=') + 1))); } catch {}
    } else if (line.startsWith('@FONTS=')) {
      // The swappable drum-font list (runtime @DRUMFONT swap). Parse into the picker's state; the
      // current-flag / @STATE.drumfont drive which one is highlighted.
      setFonts(parseFonts(line.slice(7)));
    } else if (line.startsWith('@DRUMFONT=')) {
      // Swap-complete ack from the device (it rewrote /tdsp/drumkits.ndjson for the new font). Reload
      // the catalog bypassing the cache to pull the new font's kit list, then re-hydrate every card.
      load(true).finally(() => tp.requestState());
    } else if (line.startsWith('@DRUMFONTERR=')) {
      // The swap failed (missing/too-big font); the device kept the previous font. Refresh the picker
      // so its highlight snaps back to what's actually resident.
      tp.requestFonts();
    } else if (line.startsWith('@APP=')) {
      // The device's opaque app-owned state blob (emitted with @STATE and echoed on save).
      // Restore the settings the firmware can't derive; ignore anything unrecognized.
      try { hydrateApp(JSON.parse(line.slice(5))); } catch {}
    } else if (line.startsWith('@TRK')) {
      // The engine's PATCH LIST (@TRK<i>.INSTRS=<name0>\x1f<name1>…) — the non-Dexed engines' own voice
      // picker. Matched before .INSTR= so "@TRK2.INSTRS=" doesn't get eaten by the .INSTR= regex.
      const ml = line.match(/^@TRK(\d+)\.INSTRS=(.*)$/);
      if (ml) { setTrkEngInstrs(mp => ({ ...mp, [+ml[1]]: ml[2] === '' ? [] : ml[2].split('\x1f') })); return; }
      // Sync the local instrument index from the device confirmation (@TRK<i>.INSTR=<idx>) so
      // Next/Prev step from the true current instrument, not a stale 0.
      const m = line.match(/^@TRK(\d+)\.INSTR=(\d+)/);
      if (m) setOpllIdx(mp => ({ ...mp, [+m[1]]: +m[2] }));
    } else if (line.indexOf('"conn"') >= 0 && line.indexOf('"vol"') >= 0) {
      const m = line.match(/\{.*\}/); if (m) { try { const j = JSON.parse(m[0]); setBt({ conn: !!j.conn, peer: j.peer || '' }); if (j.vol != null) setVol(j.vol); } catch {} }
    } else if (line.startsWith('@SONGP=')) {
      // Live song-playback position (permille). -1 = playback ended → reset the bar + clear ♪.
      const v = parseInt(line.slice(7), 10);
      if (v < 0) {
        setPlayer(p => ({ ...p, playing: false, prog: 0 }));
        if (manualStopRef.current) manualStopRef.current = false;   // user hit Stop → don't auto-advance
        else onSongEndRef.current();                                // natural end → Continue/Shuffle per the end-mode
      } else setPlayer(p => ({ ...p, playing: true, prog: Math.max(0, Math.min(1, v / 1000)) }));
    } else if (line.startsWith('@SONG2P=')) {
      // MIDI Player 2 position feed — same as @SONGP but for the voice-2 song player.
      const v = parseInt(line.slice(8), 10);
      if (v < 0) {
        setPlayer2(p => ({ ...p, playing: false, prog: 0 }));
        if (manualStop2Ref.current) manualStop2Ref.current = false;
        else onSong2EndRef.current();
      } else setPlayer2(p => ({ ...p, playing: true, prog: Math.max(0, Math.min(1, v / 1000)) }));
    } else if (/^@TRK\d+\.P=/.test(line)) {
      // Extra synth voices (>=2) MIDI-player position feed: @TRK<i>.P=<permille> (-1 = ended).
      const m = line.match(/^@TRK(\d+)\.P=(-?\d+)/);
      if (m) {
        const i = parseInt(m[1], 10), pv = parseInt(m[2], 10);
        setPlayerX(mp => { const cur = mp[i] ?? { song: '', playing: false, name: '', prog: 0 };
          return { ...mp, [i]: pv < 0 ? { ...cur, playing: false, prog: 0 } : { ...cur, playing: true, prog: Math.max(0, Math.min(1, pv / 1000)) } }; });
        if (pv < 0) { const r = manualStopRefX.current[i]; if (r) r.current = false; }   // clear manual-stop flag; no auto-advance for extra voices yet
      }
    } else if (line.startsWith('@ALP=')) {
      // Live audio-loop telemetry: "@ALP=<st0>,<p0>[,<st1>,<p1>...]" — one state+permille pair per loop.
      const n = line.slice(5).split(',').map(x => parseInt(x, 10));
      if (n.length >= 2 && n.every(x => !isNaN(x))) {
        setAloop(a => ({
          ...a,
          st: a.st.map((v, i) => (n[i * 2] != null ? Math.max(0, Math.min(4, n[i * 2])) : v)),
          p:  a.p.map((v, i) => (n[i * 2 + 1] != null ? n[i * 2 + 1] / 1000 : v)),
        }));
      }
    } else if (line.startsWith('@RECP=')) {
      // Live loop-recorder telemetry: "@RECP=<st1>,<p1>,<st2>,<p2>" (state + permille per voice).
      const p = line.slice(6).split(',').map(x => parseInt(x, 10));
      if (p.length >= 4 && p.every(x => !isNaN(x))) {
        setRec(r => ({ ...r, st1: Math.max(0, Math.min(4, p[0])), p1: p[1] / 1000, st2: Math.max(0, Math.min(4, p[2])), p2: p[3] / 1000 }));
      }
    } else if (line.startsWith('@BEAT=')) {
      // Live beat position from the master clock: "@BEAT=<beatInBar>/<beatsPerBar>".
      // Drives the header beat lights locked to the real device downbeat. If these stop
      // arriving (old firmware, or the clock stalled), clear the feed so BeatStrip falls
      // back to its local BPM-driven clock.
      const p = line.slice(6).split('/'); const i = parseInt(p[0], 10), n = parseInt(p[1], 10);
      if (i >= 0 && n >= 1) {
        setBeatFeed({ i, n });
        if (beatStaleRef.current) clearTimeout(beatStaleRef.current);
        // Generous window: the device emits every beat (even idle/stopped now), so this
        // only fires for genuinely-absent feeds (old firmware, reindex, disconnect). 3.5s
        // covers a full beat down past the 20 BPM floor, so a slow tempo never flickers to
        // the local clock between beats.
        beatStaleRef.current = setTimeout(() => setBeatFeed(null), 3500);
      }
    } else if (line.startsWith('@BPM=')) {
      // The device pushed a new master tempo — the tempo auto-follow: the sole piece of content to
      // start (song / loop / groove) set the master BPM to its own native tempo (unless the tempo
      // lock is on). Reflect it in the readout. App-initiated @BPM changes aren't echoed, so this
      // only fires for firmware-side changes — no feedback loop.
      const b = parseInt(line.slice(5), 10); if (b >= 20 && b <= 300) setBpm(b);
    } else if (line.startsWith('[song]')) {
      // Record the song's detected tempo (for the "Reset → song bpm" affordance), but DON'T touch
      // the master: the metronome is the tempo authority now, and songs lock to IT (not the reverse).
      const m = line.match(/([\d.]+)\s*bpm/); if (m) { const b = Math.round(parseFloat(m[1])); if (b >= 20 && b <= 300) setSongBpm(b); }
    }
    // [tp]: the transport picker can swap the instance, so this must re-bind to the new one
    // (an []-dep here would leave the subscription stranded on the dead transport).
  }), [tp]);

  async function connect(auto = false) {
    if (connectingRef.current || tp.isConnected()) return;   // live check (state may be stale) — no double-connect
    connectingRef.current = true; setConnecting(true);
    try {
      await tp.connect();
      // The user may have tapped Disconnect while the link was being established — honor it
      // and bail before we show any UI or pull the catalog (userDiscRef is the synchronous truth).
      if (userDiscRef.current) { await tp.disconnect().catch(() => {}); return; }
      setConnected(true);
      await load();
      if (userDiscRef.current) { await disconnect(); return; }   // cancelled during the catalog load
      tp.requestState();   // pull the device's real current settings → hydrate every card (see @STATE handler)
      tp.requestFonts();   // pull the swappable drum-font list (runtime @DRUMFONT builds) → the Drum Font picker
    }
    // A user cancel can surface as a connect rejection (port/scan aborted) — don't toast that.
    // The "one page owns the port" hint is Web-Serial-specific — over WiFi nothing owns a
    // port, so it would just be misleading noise.
    catch (e: any) { if (!auto && !userDiscRef.current) notify('Connect failed: ' + e + (Platform.OS === 'web' && tkind !== 'wifi' ? '\n\nClose any control.html tab (one page owns the port), then retry.' : '')); }
    finally { connectingRef.current = false; setConnecting(false); }
  }
  async function disconnect() { try { await tp.disconnect(); } catch {} setConnected(false); setConnecting(false); setLoaded(false); progBus.emit(null); setRoute('home'); }
  // Button handlers wrap connect/disconnect so a *manual* disconnect suppresses auto-reconnect
  // (else the poll below would immediately reconnect and the Disconnect button would do nothing).
  // userDiscRef is set synchronously (before the async setState lands) so connect() sees a
  // mid-connect cancel right away. A user disconnect works from the connecting state too.
  const userConnect = () => { userDiscRef.current = false; setUserDisc(false); connect(); };
  const userDisconnect = () => { userDiscRef.current = true; setUserDisc(true); disconnect(); };

  // Connecting is EXPLICIT: no auto-connect and no auto-reconnect. This poll only reflects
  // a DROPPED link into the UI (flip to "Not connected") — it never opens a connection. You
  // tap Connect App to connect, and once disconnected the app stays put until you do.
  // [tp]: re-arm against the CURRENT transport — the picker can swap it, and an []-dep would
  // leave this polling the dead instance forever (reporting a stale "connected").
  useEffect(() => {
    // Web Serial reports drops through its own read loop, so the poll was skipped on web.
    // A WiFi socket still needs it though (a dropped WS would otherwise leave the UI
    // claiming it's connected), so run it there regardless of platform.
    if (Platform.OS === 'web' && tkind !== 'wifi') return;
    let cancelled = false;
    const id = setInterval(() => {
      if (cancelled) return;
      if (!tp.isConnected()) { setConnected(false); setLoaded(false); }   // no-op if already false
    }, 4000);
    return () => { cancelled = true; clearInterval(id); };
  }, [tp, tkind]);

  // Foreground auto-reconnect. Mobile OSes suspend the JS runtime and tear down the WebSocket/
  // BLE link whenever the app is backgrounded (screen off, phone in pocket) — there's no
  // reliable way to hold a plain socket open across that, so instead of fighting it we make the
  // return trip cheap: when the app comes back to the foreground, silently re-establish the
  // link. The cached catalog (above) means this reconnect skips the multi-file download, so it
  // lands in a second or two with no visible "Loading catalog…" pass. We DON'T auto-reconnect
  // if the user explicitly tapped Disconnect (userDiscRef), and skip on web where the OS never
  // suspends a foreground tab (a Web Serial reconnect would also re-prompt for the port).
  useEffect(() => {
    if (Platform.OS === 'web') return;
    const sub = RNAppState.addEventListener('change', (st: AppStateStatus) => {
      if (st !== 'active') return;
      if (!userDiscRef.current && !connectingRef.current && !tp.isConnected()) connect(true);
    });
    return () => sub.remove();
  }, [tp]);
  // (elapsed-seconds ticker now lives inside <LoadScreen>, which mounts exactly while the
  // catalog is loading — so it no longer re-renders App every second.)
  async function load(bypassCache = false) {
    // bypassCache: after a runtime drum-font swap the device rewrites /tdsp/drumkits.ndjson but NOT
    // index.ndjson, so the differential cache would short-circuit and keep the OLD kit names — skip it.
    try { const c = await loadCatalog(tp, progBus.emit, bypassCache ? undefined : catalogCache); setCat(c); setLoaded(true); progBus.emit(null); }
    catch (e: any) {
      progBus.emit(null);
      const yes = Platform.OS === 'web'
        ? (globalThis as any).confirm?.('Catalog load failed: ' + (e?.message || e) + '\n\nRebuild it now (@REINDEX)?')
        : true;
      if (yes) await reindex();
    }
  }
  async function reindex() { setBusy(true); try { await tp.reindex(); await load(); } finally { setBusy(false); } }

  // Startup default: once the catalog is loaded, PRELOAD the first drum groove so the Drums card
  // opens with a loop ready (the first song in the list) instead of "—". Only when nothing is
  // already selected — a groove restored from @APP (remember-where-you-left-it) or one the user
  // picked wins over this default. On a fresh device boot @APP is empty, so this is exactly the
  // "at startup, preload a loop" behaviour. Selection only (not auto-played); hit ▶ to start it.
  // Preselect a real, curated first loop from the manifest (prefer a top-level /drums groove). Keep a
  // still-valid restored/user pick; fall back when the pick isn't in the catalog so the deck is never
  // stuck on "—". (The reliable groove LIST comes from the Drum Loops FolderBrowser rooted at /drums —
  // ‹/› step its published folder. A separate background @LS walk was removed: the transport allows one
  // @LS in flight, so the walk kept SUPERSEDING the FolderBrowser's own @LS -> "⚠ superseded".)
  useEffect(() => {
    if (!loaded) return;
    // Default to the first Featured groove ('/drums/01 Rock Straight.mid' = FEATURED_GROOVES[0], the
    // curated starter that's ALWAYS on the card) so the Drums card opens with a loop ready to ▶. Keep
    // any restored @APP pick or user selection. This must NOT gate on cat.grooves: the 143 KB grooves
    // manifest often doesn't load (the browser lists grooves via live @LS, not the manifest), and the
    // old `cat.grooves.length === 0 -> return` left NO loop selected on exactly those setups. The old
    // default also searched cat.grooves for a '/drums/'-prefixed path, but manifest paths are
    // '/midi/drums/...' so it never matched — the Featured starter is the reliable curated first loop.
    setDrums(d => d.sel ? d : { ...d, sel: '/drums/01 Rock Straight.mid' });
  }, [loaded]);

  // Lazy /dexed browse: fetch the current folder level via @DXLS whenever the path changes
  // (skip the synthetic '@bundled' view). @DXLS is paged (32 entries/page), so walk all pages
  // and concatenate — a folder can hold hundreds of carts. The `alive` gate drops a stale
  // fetch when the user navigates away mid-walk.
  useEffect(() => {
    // Only Dexed engines use the /dexed cart library; on others don't even call @DXLS.
    if (!loaded || vpath === '@bundled' || !cat.hasDexed) { setLevel(EMPTY_DIR); return; }
    let alive = true;
    setLibBusy(true);
    (async () => {
      try {
        const first = await tp.browseDir(vpath, 0);
        let folders = first.folders, carts = first.carts;
        for (let pg = 1; pg < first.npages && alive; pg++) {
          const more = await tp.browseDir(vpath, pg);
          folders = folders.concat(more.folders); carts = carts.concat(more.carts);
        }
        if (alive) { setLevel({ path: vpath, page: 0, npages: first.npages, folders, carts }); setLibErr(''); }
      } catch (e) { if (alive) { setLevel({ ...EMPTY_DIR, path: vpath }); setLibErr(String((e as any)?.message || e || 'browse failed')); } }
      finally { if (alive) setLibBusy(false); }
    })();
    return () => { alive = false; };
  }, [vpath, loaded, cat.hasDexed]);

  // Fetch an open cart's 32 voice names via @DXVL.
  useEffect(() => {
    if (!cart) { setCartVoices([]); return; }
    let alive = true;
    setLibBusy(true);
    tp.cartVoices(cart.rel).then(v => { if (alive) setCartVoices(v); })
      .catch(() => { if (alive) setCartVoices([]); })
      .finally(() => { if (alive) setLibBusy(false); });
    return () => { alive = false; };
  }, [cart]);

  // The voices currently listed in Synth/Voices (a cart's voices, or the bundled set).
  const voiceRef = useRef<FlatList<VItem>>(null);
  const voiceRef2 = useRef<FlatList<VItem>>(null);         // Voices-2 page has its own list ref (both pages stay mounted)
  const voiceRefX = useRef<Record<number, React.RefObject<FlatList<VItem>>>>({});   // per-extra-voice (>=2) list refs
  const browseRefX = useRef<Record<number, React.RefObject<ScrollView>>>({});
  const refFor = (m: React.MutableRefObject<Record<number, any>>, i: number) => (m.current[i] ??= React.createRef());
  const browseRef = useRef<ScrollView>(null);              // the folder-browse list
  const browseRef2 = useRef<ScrollView>(null);
  const pickerY = useRef<Record<string, number>>({});      // saved scroll offset per list, so we can restore on return
  const voiceData: VItem[] = useMemo(() =>
    cart ? cartVoices.map((vn, i) => ({ key: 'c' + cart.rel + ':' + i, label: (i + 1) + '. ' + vn, i }))
      : vpath === '@bundled' ? cat.instruments.map(v => ({ key: 'b' + v.i, label: v.name, i: v.i }))
        : [], [cart, cartVoices, vpath, cat.instruments]);
  const listId = voiceData.length ? (cart ? 'c' + cart.rel : '@bundled') : 'br:' + vpath;   // identity of the currently-shown picker list
  // A non-Dexed engine (OPLL, Plaits, any future engine) uses a FIXED patch list — its ‹/› step
  // @TRK<i>.INSTR and its browser shows @TRK<i>.INSTRS, never the Dexed /dexed cart library.
  const isPickerEngine = (eng?: string) => !!eng && eng !== 'dexed';
  // Fetch each non-Dexed track's patch list once its engine is known (from @STATE), for the browser.
  useEffect(() => {
    if (!connected) return;
    Object.keys(trkEng).forEach(k => { const i = +k; if (isPickerEngine(trkEng[i]) && !trkEngInstrs[i]) tp.trk(i, 'INSTRS'); });
  }, [connected, trkEng]);   // eslint-disable-line react-hooks/exhaustive-deps

  // Pick a voice for slot `target` (1 = main synth, 2 = the keyboard's Voices-2). Both share
  // the same browse state (cart/folder position) but have independent selection + device
  // targets (@DXVOICE/@DXPICK vs @DXVOICE2/@DXPICK2).
  const pickVoice = (it: VItem, target: number = 1) => {
    const nm = it.label.replace(/^\d+\.\s*/, '');
    const isCart = it.key[0] === 'c' && !!cart;
    const path = isCart ? '/dexed/' + cart!.rel : 'Bundled';
    if (target >= 3) {   // voices 2+ (0-based i = target-1) drive the uniform @TRK<i>.DX* interface
      const i = target - 1;
      setSelVoiceX(m => ({ ...m, [i]: it.key }));
      setTrkNames(m => ({ ...m, [i]: nm }));
      if (isCart) tp.trk(i, 'DXPICK=' + cart!.rel + '\t' + it.i); else if (it.key[0] === 'b') tp.trk(i, 'DXVOICE=' + it.i);
      return;
    }
    if (target === 2) {
      setSelVoice2(it.key);
      setVoice2(v => ({ ...v, name: nm, path }));
      if (isCart) tp.dxPick2(cart!.rel, it.i); else if (it.key[0] === 'b') tp.dxVoice2(it.i);
      return;
    }
    setSelVoice(it.key); setSelVoiceName(nm);
    if (isCart) { setSelVoicePath('/dexed/' + cart!.rel); tp.dxPick(cart!.rel, it.i); }
    else if (it.key[0] === 'b') { setSelVoicePath('Bundled'); tp.dxVoice(it.i); }
  };
  const stepVoice = (dir: number, target: number = 1) => {
    // ANY non-Dexed engine (OPLL ROM voices, Plaits models, any future engine) has a FIXED patch list
    // and NO /dexed cart library — step @TRK<i>.INSTR with WRAP over the engine's own instrument count
    // (@STATE.ninstr), never the Dexed catalog. The name comes back via @STATE.tracks[].name. This one
    // branch covers every engine, so a new synth needs no new code (fixes OPLL AND Plaits AND future).
    const oi = target - 1;
    if (isPickerEngine(trkEng[oi])) {
      const n = trkNinstr[oi] || 0;
      const cur = opllIdx[oi] ?? 0;
      const ni = n > 0 ? ((cur + dir) % n + n) % n : Math.max(0, cur + dir);   // wrap if count known, else clamp (device bounds it)
      setOpllIdx(m => ({ ...m, [oi]: ni }));
      tp.trk(oi, 'INSTR=' + ni);
      tp.requestState();
      return;
    }
    const sel = target >= 3 ? (selVoiceX[target - 1] ?? '') : target === 2 ? selVoice2 : selVoice;
    // In a visible list (a cart's voices or the bundled set), step within it so the
    // selection stays scrolled into view.
    if (voiceData.length) {
      const idx = voiceData.findIndex(d => d.key === sel);
      const ni = Math.max(0, Math.min(voiceData.length - 1, (idx < 0 ? 0 : idx) + dir));
      if (voiceData[ni]) pickVoice(voiceData[ni], target);
      return;
    }
    // No list open (browsing folders): step within the bundled set. Crossing /dexed cart
    // boundaries isn't possible now that the library is browsed lazily (not held in RAM),
    // so to step through SD voices, open a cart first.
    if (sel[0] !== 'c' && cat.instruments.length) {
      const idx = cat.instruments.findIndex(v => 'b' + v.i === sel);
      const ni = Math.max(0, Math.min(cat.instruments.length - 1, (idx < 0 ? 0 : idx) + dir));
      const v = cat.instruments[ni]; if (v) pickVoice({ key: 'b' + v.i, label: v.name, i: v.i }, target);
    }
  };
  useEffect(() => {   // keep the Voices-2 selection in view on its own list
    const idx = voiceData.findIndex(d => d.key === selVoice2);
    if (idx >= 0) { const t = setTimeout(() => { try { voiceRef2.current?.scrollToIndex({ index: idx, animated: true, viewPosition: 0.5 }); } catch {} }, 60); return () => clearTimeout(t); }
  }, [selVoice2, voiceData]);
  useEffect(() => {   // keep the selected voice in view (on pick + on list change)
    const idx = voiceData.findIndex(d => d.key === selVoice);
    if (idx >= 0) { const t = setTimeout(() => { try { voiceRef.current?.scrollToIndex({ index: idx, animated: true, viewPosition: 0.5 }); } catch {} }, 60); return () => clearTimeout(t); }
  }, [selVoice, voiceData]);
  useEffect(() => {   // on returning to the Synth page, restore the picker's scroll offset
    if (route !== 'synth') return;
    const y = pickerY.current[listId] || 0;
    const t = setTimeout(() => {
      if (voiceData.length) { try { voiceRef.current?.scrollToOffset({ offset: y, animated: false }); } catch {} }
      else { try { browseRef.current?.scrollTo({ y, animated: false }); } catch {} }
    }, 40);
    return () => clearTimeout(t);
  }, [route]);

  const stepBpm = (delta: number) => { const b = Math.max(20, Math.min(300, Math.round(bpm) + delta)); setBpm(b); tp.masterBpm(b); };
  const stepVol = (delta: number) => { const v = Math.max(0, Math.min(100, Math.round(vol) + delta)); setVol(v); tp.masterVolume(v); };
  // TAC5212 DAC high-pass filter. Picking a preset sets the mode; the Enable switch
  // toggles between Off and the last non-off cutoff (default 12 Hz) so a disable is undoable.
  const setHpfMode = (mode: number) => { setHpf(mode); if (mode) lastHpfRef.current = mode; tp.dacHpf(mode); };
  const toggleHpf = (on: boolean) => setHpfMode(on ? (lastHpfRef.current || 2) : 0);
  // Select an arp pattern. Entering User Sequence also (re)uploads the current step
  // table, since the device doesn't persist/echo it — the app is its source of truth.
  const selectPattern = (i: number) => {
    setArp(a => ({ ...a, pat: i }));
    tp.arpPattern(i);
    if (i === PAT_USER_SEQUENCE) tp.arpSequence(seq);
    setArpPresetId('');   // manual pattern pick diverges from any applied library preset
  };
  const stepArpPat = (dir: number) => selectPattern((arp.pat + dir + ARP_PAT.length) % ARP_PAT.length);
  // A step-grid edit: keep the app copy and push it live. If the arp isn't already on the
  // User Sequence pattern, switch to it so the edit is immediately audible.
  const applySeq = (steps: SeqStep[]) => {
    setSeq(steps);
    tp.arpSequence(steps);
    if (arp.pat !== PAT_USER_SEQUENCE) { setArp(a => ({ ...a, pat: PAT_USER_SEQUENCE })); tp.arpPattern(PAT_USER_SEQUENCE); }
    setArpPresetId('');
  };
  // Apply a preset from the 238-entry library: push its params to the device, reflect them
  // in the arp UI (pattern/rate/octaves/latch pills), and adopt its step table if any.
  const applyPreset = (p: ArpPreset) => {
    const st = applyArpPreset(tp, p, 1, arp.latch);   // keep the user's latch (sticky across presets)
    setArp(a => ({ ...a, pat: st.pat, rate: st.rate, oct: st.oct, latch: st.latch }));
    if (st.seq) setSeq(st.seq);
    setArpPresetId(p.id);
  };
  // Header ‹ › steps within the ACTIVE editor: through the library in Preset mode, through
  // the pattern list in Manual mode — so the arrows never touch the tab you're not using.
  const stepPreset = (dir: number) => {
    if (!ARP_LIBRARY.length) return;
    const idx = ARP_LIBRARY.findIndex(p => p.id === arpPresetId);
    const ni = (idx < 0 ? (dir > 0 ? -1 : 0) : idx) + dir;
    applyPreset(ARP_LIBRARY[(ni + ARP_LIBRARY.length) % ARP_LIBRARY.length]);
  };
  const stepArpNav = (dir: number) => (arpMode === 'preset' ? stepPreset(dir) : stepArpPat(dir));
  // Reset the arp to a PLAIN manual config: clear every hidden extra a preset may have set
  // (scale, velocity curve, step mask, MPE mode / output channel, transpose, repeat…) back to
  // engine defaults while keeping the pattern/rate/octaves/latch the pills show. This is why
  // manual can "stop working" after a preset — an MPE preset routes the arp to scatter channels
  // and a masked preset mutes steps, and the manual pills can't undo those. One @ARPPRESET line
  // with default extras restores a clean, audible arp.
  const resetArpManual = () => {
    tp.arpPreset({
      pat: arp.pat, rate: ARP_RATES[arp.rate].fw, gatePct: 50, swingPct: 50,
      oct: arp.oct, octMode: 0, latch: arp.latch, velMode: 0, velFixed: 100, velAccent: 127,
      stepMask: -1, stepLength: 16, mpeMode: 0, outCh: 1, scatterBase: 2, scatterCount: 4,
      scale: 0, scaleRoot: 0, transpose: 0, repeat: 1,
    });
    setArpPresetId('');
  };
  // Entering Manual mode ALWAYS lands on a clean slate — otherwise a preset's invisible extras
  // (scatter channel, step mask, scale…) linger and the manual controls appear dead.
  const enterManualMode = () => { setArpMode('manual'); resetArpManual(); };
  // Independent transport: Play (re)starts — if already on it restarts the cycle from step 0
  // (@ARPRESTART) rather than being a no-op; Stop bypasses. So you can re-trigger a running arp.
  const playArp = () => { if (arp.on) tp.arpRestart(); else { setArp(a => ({ ...a, on: true })); tp.arpOn(true); } };
  const stopArp = () => { setArp(a => ({ ...a, on: false })); tp.arpOn(false); };
  const activePresetName = ARP_LIBRARY.find(p => p.id === arpPresetId)?.name || '';

  // --- Arp 2 (the keyboard voice's own arp) — the exact same handler set, targeting the
  // @ARP2* commands + the arp2 UI state, so the Arpeggiator 2 card is a full clone. ---
  const selectPattern2 = (i: number) => { setArp2(a => ({ ...a, pat: i })); tp.arp2Pattern(i); if (i === PAT_USER_SEQUENCE) tp.arp2Sequence(seq2); setArp2PresetId(''); };
  const stepArpPat2 = (dir: number) => selectPattern2((arp2.pat + dir + ARP_PAT.length) % ARP_PAT.length);
  const applySeq2 = (steps: SeqStep[]) => { setSeq2(steps); tp.arp2Sequence(steps); if (arp2.pat !== PAT_USER_SEQUENCE) { setArp2(a => ({ ...a, pat: PAT_USER_SEQUENCE })); tp.arp2Pattern(PAT_USER_SEQUENCE); } setArp2PresetId(''); };
  const applyPreset2 = (p: ArpPreset) => { const st = applyArpPreset(tp, p, 2, arp2.latch); setArp2(a => ({ ...a, pat: st.pat, rate: st.rate, oct: st.oct, latch: st.latch })); if (st.seq) setSeq2(st.seq); setArp2PresetId(p.id); };
  const stepPreset2 = (dir: number) => { if (!ARP_LIBRARY.length) return; const idx = ARP_LIBRARY.findIndex(p => p.id === arp2PresetId); const ni = (idx < 0 ? (dir > 0 ? -1 : 0) : idx) + dir; applyPreset2(ARP_LIBRARY[(ni + ARP_LIBRARY.length) % ARP_LIBRARY.length]); };
  const stepArpNav2 = (dir: number) => (arp2Mode === 'preset' ? stepPreset2(dir) : stepArpPat2(dir));
  const resetArpManual2 = () => { tp.arp2Preset({ pat: arp2.pat, rate: ARP_RATES[arp2.rate].fw, gatePct: 50, swingPct: 50, oct: arp2.oct, octMode: 0, latch: arp2.latch, velMode: 0, velFixed: 100, velAccent: 127, stepMask: -1, stepLength: 16, mpeMode: 0, outCh: 1, scatterBase: 2, scatterCount: 4, scale: 0, scaleRoot: 0, transpose: 0, repeat: 1 }); setArp2PresetId(''); };
  const enterManualMode2 = () => { setArp2Mode('manual'); resetArpManual2(); };
  const playArp2 = () => { if (arp2.on) tp.arp2Restart(); else { setArp2(a => ({ ...a, on: true })); tp.arp2On(true); } };
  const stopArp2 = () => { setArp2(a => ({ ...a, on: false })); tp.arp2On(false); };
  const activePreset2Name = ARP_LIBRARY.find(p => p.id === arp2PresetId)?.name || '';

  // One arp "slot" bundles a filter's state + handlers so the same body/value/actions render
  // both the main arp and Arp 2. arpSlot(1) = main (@ARP*), arpSlot(2) = keyboard (@ARP2*).
  type ArpSlotT = {
    arp: { on: boolean; pat: number; rate: number; oct: number; latch: boolean };
    mode: 'preset' | 'manual'; setMode: (m: 'preset' | 'manual') => void;
    presetId: string; activeName: string; seq: SeqStep[];
    play: () => void; stop: () => void; stepNav: (d: number) => void;
    selectPattern: (i: number) => void; applyPreset: (p: ArpPreset) => void; applySeq: (st: SeqStep[]) => void;
    enterManual: () => void; resetManual: () => void;
    setRate: (i: number) => void; setOct: (n: number) => void; setLatch: (v: boolean) => void;
  };
  const arpSlot1: ArpSlotT = {
    arp, mode: arpMode, setMode: setArpMode, presetId: arpPresetId, activeName: activePresetName, seq,
    play: playArp, stop: stopArp, stepNav: stepArpNav,
    selectPattern, applyPreset, applySeq, enterManual: enterManualMode, resetManual: resetArpManual,
    setRate: i => { setArp(a => ({ ...a, rate: i })); tp.arpRate(ARP_RATES[i].fw); setArpPresetId(''); },
    setOct: n => { setArp(a => ({ ...a, oct: n })); tp.arpOctaves(n); setArpPresetId(''); },
    setLatch: v => { setArp(a => ({ ...a, latch: v })); tp.arpLatch(v); setArpPresetId(''); },
  };
  const arpSlot2: ArpSlotT = {
    arp: arp2, mode: arp2Mode, setMode: setArp2Mode, presetId: arp2PresetId, activeName: activePreset2Name, seq: seq2,
    play: playArp2, stop: stopArp2, stepNav: stepArpNav2,
    selectPattern: selectPattern2, applyPreset: applyPreset2, applySeq: applySeq2, enterManual: enterManualMode2, resetManual: resetArpManual2,
    setRate: i => { setArp2(a => ({ ...a, rate: i })); tp.arp2Rate(ARP_RATES[i].fw); setArp2PresetId(''); },
    setOct: n => { setArp2(a => ({ ...a, oct: n })); tp.arp2Octaves(n); setArp2PresetId(''); },
    setLatch: v => { setArp2(a => ({ ...a, latch: v })); tp.arp2Latch(v); setArp2PresetId(''); },
  };
  const arpValue = (A: ArpSlotT) => (A.arp.on ? '' : '(off)  ') + (A.mode === 'preset'
    ? (A.activeName || 'Preset — none picked')
    : ARP_PAT[A.arp.pat] + '  ·  ' + ARP_RATES[A.arp.rate].label);
  const arpActions = (A: ArpSlotT) => (<>
    <HdrBtn label="‹" stop onPress={() => A.stepNav(-1)} />
    <HdrBtn label="›" stop onPress={() => A.stepNav(1)} />
    <HdrBtn label="▶" onPress={A.play} active={A.arp.on} />
    <HdrBtn label="■" stop onPress={A.stop} />
  </>);
  const arpBody = (A: ArpSlotT) => (
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
  // Merge a patch into the persisted app-state and push the whole blob to the device (@APP=)
  // so it survives an app reload/reconnect. The device stores it opaquely; the app owns it.
  const persistApp = (patch: Partial<AppState>) => { appStateRef.current = { ...appStateRef.current, ...patch }; tp.saveAppState(appStateRef.current); };

  // Each deck's CURRENT FOLDER file list (ordered, as {playArg, displayName}), published by that
  // deck's FolderBrowser as the user navigates. This is what makes ‹/› and auto-advance step within
  // the folder you're viewing instead of the flat catalog. Keyed by deckKey ('song1'/'song2'/'drum'/
  // 'x<v>'); a deck whose browser was never opened just isn't in the map (step falls back to catalog).
  const folderFilesRef = useRef<Record<string, { arg: string; name: string }[]>>({});

  // ===== MIDI player deck =====================================================================
  // ONE definition of what a MIDI player IS — state + every handler — parameterized by the voice
  // it drives. Deck 1 is voice 1 (@SONG*), deck 2 is voice 2 (@SONG2*). The renderers below
  // (playerActions / playerSongBody / playerBody / playerValue) take a deck, so both players are
  // the SAME component: a change to one changes both, and a third synth is one makeSongDeck()
  // call + one section entry — no cloned UI, no cloned transport logic.
  type PlayerT = { song: string; playing: boolean; name: string; prog: number };
  type SongWire = { play: (arg: string) => void; restart: (arg: string) => void; stop: () => void; loop: (on: boolean) => void };
  type InjFolder = { name: string; leaves: { name: string; arg: string }[] };
  type SongDeckT = {
    v: 1 | 2;
    player: PlayerT; endMode: EndMode;
    vol: number; onVol: (n: number) => void; commitVol: (n: number) => void; volNote?: string;
    setSong: (name: string) => void;
    playFile: (arg: string, disp: string) => void;   // FolderBrowser tap: restart this file/baked song now (arg = full SD path or baked name)
    play: () => void; stop: () => void; step: (dir: number) => void;
    applyEnd: (m: EndMode) => void; cycleEnd: () => void;
    onFolderList: (files: { arg: string; name: string }[]) => void;   // FolderBrowser publishes the current folder here
    onNaturalEnd: () => void;   // wired into the device's position feed (@SONGP=-1 / @SONG2P=-1)
    // Per-track browse target, so the SAME player component serves any track. Every deck now roots
    // the browser at /midi so any synth can reach ALL folders (songs, drums, loops, tests, custom),
    // not just one — undefined falls back to the /midi default (see FolderBrowser root below).
    // noEndMode hides the end-mode row (a drum groove always loops). This makes the deck replicable
    // across tracks.
    browseRoot?: string; injectFolders?: InjFolder[]; noEndMode?: boolean; filesFirst?: boolean;
  };
  const makeSongDeck = (cfg: {
    v: 1 | 2;
    player: PlayerT; setPlayer: React.Dispatch<React.SetStateAction<PlayerT>>;
    endMode: EndMode; setEndMode: (m: EndMode) => void; persistKey: 'end' | 'end2';
    manualStopRef: React.MutableRefObject<boolean>;
    vol: number; onVol: (n: number) => void; commitVol: (n: number) => void; volNote?: string;
    wire: SongWire;
    // Called whenever the SELECTED song/groove changes (tap, ‹/›, auto-advance) — even while
    // stopped. Lets a track that the device can't report (the drum groove) remember its pick in
    // @APP so it survives leaving the page / reconnecting. Undefined for song decks (the device
    // reports their current song in @STATE, so there's nothing extra to persist).
    persistSel?: (arg: string) => void;
    deckKey: string;   // folderFilesRef key so this deck's ‹/› step within the folder its browser shows
    // Per-track browse target + step catalog, so the SAME deck serves any track. Browser default = /midi.
    catalog?: Song[]; browseRoot?: string; injectFolders?: InjFolder[]; noEndMode?: boolean; filesFirst?: boolean;
    // Deterministic stepping: ‹/› + Play walk the `catalog` (this deck's own list) and IGNORE whatever
    // folder the browser last showed. The drum card uses this so its next/prev always cycle drum
    // grooves — not the /midi folder you happened to browse into (which broke drum ‹/›).
    stepCatalogOnly?: boolean;
  }): SongDeckT => {
    const { player: P, setPlayer: setP, endMode: em, wire } = cfg;
    const persistSel = cfg.persistSel ?? (() => {});
    const catalog = cfg.catalog ?? cat.songs;   // the list ‹ ›/auto-advance step through (songs or grooves)
    // The shared "continue rules": which song a skip (‹ ›) or a natural end advances to, per the
    // end-mode. Shuffle → a random *other* song; every other mode → the linear neighbour, wrapping
    // both ways. Both the transport buttons and the auto-advance route through this.
    // Song identity is the play ARG (full SD path for an SD song, the bare name for a baked one) —
    // i.e. songArg(row) — so it round-trips through wire.restart and matches the FolderBrowser's
    // fullPath. Auto-advance / ‹ › still step through the whole flat catalog (cat.songs); the
    // browser's per-folder view is a separate, richer picker (see note in playerSongBody).
    const pickNext = (dir: number): Song | null => {
      // Prefer the FOLDER the user is browsing (its FolderBrowser publishes an ordered file list): ‹/›
      // and auto-advance step within THAT folder — matching what's on screen. Fall back to the flat
      // catalog only when no folder list exists (a baked song, or the browser was never opened).
      const folder = cfg.stepCatalogOnly ? undefined : folderFilesRef.current[cfg.deckKey];
      if (folder && folder.length) {
        const fi = folder.findIndex(f => f.arg === P.song);
        if (em === 'shuffle' && folder.length > 1) {
          let r = fi; while (r === fi) r = Math.floor(Math.random() * folder.length);
          return { name: folder[r].name, file: folder[r].arg };
        }
        const b = fi < 0 ? (dir > 0 ? -1 : 0) : fi;
        const f = folder[((b + dir) % folder.length + folder.length) % folder.length];
        return { name: f.name, file: f.arg };
      }
      const songs = catalog;
      if (!songs.length) return null;
      const idx = songs.findIndex(sg => songArg(sg) === P.song);
      if (em === 'shuffle' && songs.length > 1) {
        let r = idx; while (r === idx) r = Math.floor(Math.random() * songs.length);   // never repeat the current song
        return songs[r];
      }
      const base = idx < 0 ? (dir > 0 ? -1 : 0) : idx;
      return songs[((base + dir) % songs.length + songs.length) % songs.length];       // wrap both ways
    };
    const playOf = (sg: Song) => { wire.play(songArg(sg)); persistSel(songArg(sg)); setP(p => ({ ...p, song: songArg(sg), playing: true, name: sg.name, prog: -1 })); };
    // End-of-song mode. Only 'repeat' arms the firmware's seamless loop; the rest let the song end
    // (the device emits its <player>P=-1) and we advance app-side. Persisted on the device.
    const applyEnd = (m: EndMode) => { cfg.setEndMode(m); wire.loop(m === 'repeat'); persistApp({ [cfg.persistKey]: m } as Partial<AppState>); };
    return {
      v: cfg.v, player: P, endMode: em,
      vol: cfg.vol, onVol: cfg.onVol, commitVol: cfg.commitVol, volNote: cfg.volNote,
      setSong: (name: string) => setP(p => ({ ...p, song: name })),
      // FolderBrowser tap: restart THIS file/baked song now (arg = full SD path, or a baked name).
      playFile: (arg: string, disp: string) => { wire.restart(arg); persistSel(arg); setP(p => ({ ...p, song: arg, playing: true, name: disp, prog: -1 })); },
      // Play = restart the CURRENTLY SELECTED track from the top on a fresh downbeat (whatever the
      // browser/‹› last selected — P.song). Falls back to the folder's first file, then the flat
      // catalog, so Play always does something. prog -1 until the device reports position.
      play: () => {
        const folder = cfg.stepCatalogOnly ? undefined : folderFilesRef.current[cfg.deckKey];
        const arg = P.song || folder?.[0]?.arg || (catalog[0] ? songArg(catalog[0]) : '');
        if (!arg) return;
        const nm = folder?.find(f => f.arg === arg)?.name || catalog.find(x => songArg(x) === arg)?.name || P.name || arg;
        wire.restart(arg); persistSel(arg); setP(p => ({ ...p, song: arg, playing: true, name: nm, prog: -1 }));
      },
      stop: () => { cfg.manualStopRef.current = true; wire.stop(); setP(p => ({ ...p, playing: false, prog: 0 })); },
      // ‹/› step through the catalog. Persist the new pick even when stopped, so leaving the page
      // (or reconnecting) reopens on the groove you stepped to — not the last one that played.
      step: (dir: number) => { const sg = pickNext(dir); if (!sg) return; persistSel(songArg(sg)); setP(p => { if (p.playing) { wire.restart(songArg(sg)); return { ...p, song: songArg(sg), name: sg.name }; } return { ...p, song: songArg(sg), name: sg.name }; }); },
      applyEnd, cycleEnd: () => { const i = END_MODES.findIndex(m => m.key === em); applyEnd(END_MODES[(i + 1) % END_MODES.length].key); },
      onFolderList: files => { folderFilesRef.current[cfg.deckKey] = files; },
      // Runs when a song finishes on its own (not a manual Stop). 'stop' does nothing; 'repeat'
      // loops in firmware; continue/shuffle advance per the same pickNext rules.
      onNaturalEnd: () => { if (em === 'continue' || em === 'shuffle') { const nx = pickNext(1); if (nx) playOf(nx); } },
      browseRoot: cfg.browseRoot, injectFolders: cfg.injectFolders, noEndMode: cfg.noEndMode, filesFirst: cfg.filesFirst,
    };
  };
  const songDeck1 = makeSongDeck({
    v: 1, player, setPlayer, endMode, setEndMode, persistKey: 'end', manualStopRef, deckKey: 'song1',
    vol: songVol, onVol: setSongVol, commitVol: v => tp.songVol(v),
    wire: { play: a => tp.songPlay(a), restart: a => tp.songRestart(a), stop: () => tp.stopSong(), loop: on => tp.songLoop(on) },
  });
  const songDeck2 = makeSongDeck({
    v: 2, player: player2, setPlayer: setPlayer2, endMode: endMode2, setEndMode: setEndMode2, persistKey: 'end2', manualStopRef: manualStop2Ref, deckKey: 'song2',
    // Player 2's level IS the voice-2 bus (the two halves share one mixer), so it moves the same
    // fader as the Synth / Voices 2 card rather than owning a private one.
    vol: voice2.vol, onVol: v => setVoice2(x => ({ ...x, vol: v })), commitVol: v => tp.voice2Vol(v),
    volNote: 'Plays on the Synthesizer B (voice-2) side — layered with Player 1. Volume is the shared voice-2 level.',
    wire: { play: a => tp.song2Play(a), restart: a => tp.song2Restart(a), stop: () => tp.stopSong2(), loop: on => tp.song2Loop(on) },
  });
  // Kept fresh in refs so the one-time position listeners never see a stale mode/song/catalog.
  onSongEndRef.current = songDeck1.onNaturalEnd;
  onSong2EndRef.current = songDeck2.onNaturalEnd;
  const stopDrums = () => { tp.stopDrums(); setDrums(d => ({ ...d, playing: null })); };
  // ---- DRUM DECK — the groove player as the SAME reusable deck the synths use, so Drums is a
  // Track peer: it browses /midi (all folders), plays via @DRUMF, always loops. A thin PlayerT VIEW over the
  // drums {sel,playing} state gives the shared deck API without a parallel state store. This is the
  // whole point — a drum track is just another synth with props (root, catalog, always-loop).
  const drumPlayerView: PlayerT = { song: drums.sel ?? '', playing: !!drums.playing, name: drums.playing || grooveDisp(drums.sel) || '—', prog: -1 };
  const setDrumPlayerView: React.Dispatch<React.SetStateAction<PlayerT>> = upd => setDrums(d => {
    const cur: PlayerT = { song: d.sel ?? '', playing: !!d.playing, name: d.playing || grooveDisp(d.sel) || '—', prog: -1 };
    const nx = typeof upd === 'function' ? (upd as (p: PlayerT) => PlayerT)(cur) : upd;
    return { ...d, sel: nx.song || null, playing: nx.playing ? (nx.name || grooveDisp(nx.song)) : null };
  });
  // The card keeps drums in TWO places: the big genre library at /midi/drums (browsed live) and a small
  // curated starter set at /drums (NOT in the firmware's /midi/drums scan, so it never reaches the
  // manifest). Surface those curated grooves as a folder at the TOP of the drum browser — the same
  // "folder of built-ins" pattern as the song browser — so they stay one tap away above the genre
  // folders. They play by verbatim path (@DRUMF uses a leading '/' as-is). Fixed shipped set → static.
  const FEATURED_GROOVES = ['01 Rock Straight', '02 Funk 16ths', '03 Pop Backbeat',
    '04 Hip-Hop Boom Bap', '05 Jazz Swing Ride', '06 Latin Clave']
    .map(n => ({ name: n, arg: '/drums/' + n + '.mid' }));
  const drumInjectFolders = [{ name: '★ Featured', leaves: FEATURED_GROOVES }];
  // Deck catalog = the grooves manifest, FALLING BACK to the Featured set. ‹/› (and Play) use this
  // catalog whenever the browser hasn't published a folder yet — so next/prev/loop-name work from the
  // FIRST load, not only after you open the browser or hand-pick a loop. Also covers the common case
  // where the 143 KB /midi/drums manifest didn't load (then cat.grooves is empty).
  const drumSongs: Song[] = (cat.grooves.length
    ? cat.grooves.map(g => ({ name: g.name, file: g.path }))
    : FEATURED_GROOVES.map(g => ({ name: g.name, file: g.arg })));
  const drumDeck = makeSongDeck({
    v: 1, player: drumPlayerView, setPlayer: setDrumPlayerView, endMode: 'repeat', setEndMode: () => {}, persistKey: 'end', manualStopRef: drumStopRef, deckKey: 'drum',
    vol: drumVol, onVol: setDrumVol, commitVol: v => tp.drumVol(v),
    wire: { play: a => tp.playGrooveFile(a), restart: a => tp.playGrooveFile(a), stop: () => tp.stopDrums(), loop: () => {} },
    // The device reports the drum KIT in @STATE but not which groove is picked, so remember the
    // selection ourselves (in @APP) on every pick/step — that's what lets Drums reopen where you left it.
    persistSel: a => persistApp({ groove: a }),
    // Root at /midi/drums: a live @LS shows the 25 genre folders (country/swing/funk/…) — the big
    // library. The ★ Featured folder (injected above) sits at the top with the 6 curated /drums starter
    // grooves. So: Featured one tap away, full genre library right below. No manifest dependency.
    catalog: drumSongs, browseRoot: '/midi/drums', injectFolders: drumInjectFolders, noEndMode: true,   // a groove always loops
  });
  // Master transport Play/Stop. Play (@METRO=1) starts the clock + defines the downbeat if idle
  // (idempotent while running); everything locks to it. Stop (@METRO=0) halts + clears the stage.
  const playMetro = () => { setMetro(m => ({ ...m, on: true })); tp.metronome(true); };
  // Global Stop halts the whole transport in firmware — both players + drums. Flag the resulting
  // @SONGP=-1 / @SONG2P=-1 as manual stops so they don't auto-advance to the next song, and clear
  // the player/drum UI optimistically (the device position feed confirms).
  const stopMetro = () => {
    manualStopRef.current = true; manualStop2Ref.current = true;
    setMetro(m => ({ ...m, on: false }));
    setPlayer(p => ({ ...p, playing: false, prog: 0 }));
    setPlayer2(p => ({ ...p, playing: false, prog: 0 }));
    setDrums(d => ({ ...d, playing: null }));
    tp.metronome(false);
  };

  const headerStatus = !connected ? 'Not connected' :
    [cat.engine || 'synth', cat.drumEngine ? cat.drumEngine + ' drums' : '', '♩ ' + Math.round(bpm) + ' BPM', TP_LABEL[tp.name], bt.conn ? 'BT:' + (bt.peer || 'on') : '', drums.playing ? '♪ ' + drums.playing : ''].filter(Boolean).join('  ·  ');

  // ===== the sections: one entry drives both its homepage card and its page. =====
  // `value`/`status` = the subtitle; `actions` = the header controls; `body` = the page.
  // `parent` (a section id) makes this a SUB-page reached from that parent's submenu instead of a
  // home card — its Back button returns to the parent, not home (see the render's onBack).
  type Section = { id: string; title: string; show: boolean; value?: string; status?: string; subtitle?: React.ReactNode; progress?: number; actions?: React.ReactNode; foot?: React.ReactNode; body: React.ReactNode; fullHeight?: boolean; accent?: string; tint?: string; topRight?: React.ReactNode; parent?: string; disabledReason?: string; asCard?: boolean };

  // "Dexed: <name>" on the Dexed synth cards — mirrors the firmware's own prefixes on the other
  // engines ("OPLL: X" HeteroOpll.h, "Plaits: X" HeteroPlaits.h, …) so a hetero build reads
  // consistently. Applied at DISPLAY time (not baked into the stored name). ONLY the Dexed voices
  // (eng absent/inferred, or "dexed") get the prefix here — every other engine's name ALREADY carries
  // its tag from the firmware, so re-prefixing double-labels it ("Dexed: Plaits: …"). Prefix only on a
  // Dexed-capable build.
  const ENGINE_LABELS: Record<string, string> = { opll: 'OPLL', plaits: 'Plaits', opl3: 'OPL3', ymfm: 'YM2151', rings: 'Rings', va: 'VA', sf2: 'SF2' };
  const engName = (name: string, eng?: string) => {
    if (!eng || eng === 'dexed') return cat.hasDexed ? 'Dexed: ' + name : name;
    const lbl = ENGINE_LABELS[eng] || eng.toUpperCase();
    if (name.startsWith(lbl + ':')) return name;   // firmware already labels the voice with its engine (e.g. Plaits)
    // Show ENGINE: voice — the ENGINE is what the tag says (e.g. opll->OPLL), not the voice-BANK prefix the
    // firmware groups voices under ("ROM: "/"PSS-140: " for OPLL, "GM: " for GM banks). Strip that bank.
    return lbl + ': ' + name.replace(/^(ROM|PSS-140|GM):\s*/, '');
  };
  // The card/page subtitle: the currently-loaded instrument if one is picked, else a
  // summary of where the browser is (folder name or catalog counts).
  const synthValue = selVoiceName
    ? engName(selVoiceName)
    : (vpath === '@bundled' ? 'Bundled' : vpath ? (vpath.split('/').pop() || '') : (cat.instruments.length ? cat.instruments.length + ' voices + SD library' : 'Library'));
  // When an instrument is loaded, show its name AND full location (folder/cart path). The
  // path truncates from the LEFT (ellipsizeMode "head") so the deepest folder + cart stay
  // visible. No voice picked yet → undefined, so the card falls back to the browse summary.
  const synthSubtitle = selVoiceName ? (
    <>
      <Text style={s.drawerValue} numberOfLines={1}>{engName(selVoiceName)}</Text>
      {!!selVoicePath && <Text style={s.pathLine} numberOfLines={1} ellipsizeMode="head">{selVoicePath}</Text>}
    </>
  ) : undefined;
  // Available polyphony for a synth voice: Dexed pool voices report their engine count (@STATE.pool),
  // and normal mode plays 2 notes per engine. OPLL voices don't expose a count → omit.
  const polyNote = (vIdx: number): string =>
    (pool.has && trkEng[vIdx] !== 'opll' && (pool.engines[vIdx] || 0) > 0) ? pool.engines[vIdx] * 2 + '-voice' : '';
  // Shared synth-card subtitle (used by ALL Synth A-D cards so they're identical): the instrument
  // (engine-prefixed), its library path, the loaded MIDI-player song (♪), and the voice/poly count.
  const synthCardSub = (vIdx: number, instr?: string, path?: string, song?: string): React.ReactNode => {
    // Prefer the live player name; fall back to @STATE tracks[].song so it's populated on FIRST open
    // (esp. C/D, whose live playerX isn't in @STATE). Always render a song line — "♪ No song" when empty.
    const sg = song || trkSong[vIdx] || '';
    const extra = [sg ? '♪ ' + sg : '♪ No song', polyNote(vIdx)].filter(Boolean).join('   ·   ');
    return (
      <>
        {!!instr && <Text style={s.drawerValue} numberOfLines={1}>{engName(instr, trkEng[vIdx])}</Text>}
        {!!path && path !== 'Bundled' && <Text style={s.pathLine} numberOfLines={1} ellipsizeMode="head">{path}</Text>}
        <Text style={s.pathLine} numberOfLines={1}>{extra}</Text>
      </>
    );
  };

  // Synth/Voices navigation: breadcrumb trail + an up-one-level control. `cart` selected
  // ⇒ showing that cart's voices; `vpath` = the /dexed folder path ('@bundled' = bundled set).
  const atRoot = !cart && !vpath;
  const goUp = () => {
    if (cart) setCart(null);
    else if (vpath === '@bundled') setVpath('');
    else setVpath(vpath.split('/').slice(0, -1).join('/'));
  };
  const crumbs: { label: string; go: () => void }[] = [{ label: 'Voices', go: () => { setCart(null); setVpath(''); } }];
  if (vpath === '@bundled') crumbs.push({ label: 'Bundled', go: () => setCart(null) });
  else if (vpath) { let acc = ''; for (const seg of vpath.split('/')) { acc = acc ? acc + '/' + seg : seg; const p = acc; crumbs.push({ label: seg, go: () => { setCart(null); setVpath(p); } }); } }
  if (cart) crumbs.push({ label: cart.name, go: () => {} });

  // USB-keyboard ownership across ALL synth tracks (Thread C live-MIDI subscription). The header
  // keyboard glyph on every synth card shows who hears the USB MIDI keyboard: WHITE = this synth is
  // the one listening; GREY = tap to move the keyboard here. Exclusive — claiming USB for one synth
  // strips it from the others (a DIN assignment on another synth is preserved). Reads/writes the same
  // trkSubs the per-synth MIDI-Input selector uses (@TRK<i>.SRC), so the glyph and that selector stay
  // in sync. Replaces the old @VOICE2 split toggle here: the pool split is permanent now, so USB
  // routing is the real per-voice control (and it now works for Synth B/C/D, not just A).
  const srcHasUsb = (src: string) => src === 'usb' || src === 'multi' || src === 'all';
  const usbOwner = (i: number) => srcHasUsb(trkSubs[i]?.src ?? 'none');
  const cmdForSrc = (src: string) => (src === 'multi' ? 'all' : src);   // readback 'multi' -> command 'all'
  const claimUsb = (i: number) => {
    // New readback src per synth track: the target gains USB (din -> multi, else usb); every other
    // synth drops USB (multi -> din, usb -> none). Only changed tracks are written to the device.
    const nextFor = (t: number, cur: string) =>
      t === i ? ((cur === 'din' || cur === 'multi' || cur === 'all') ? 'multi' : 'usb')
              : (srcHasUsb(cur) ? ((cur === 'multi' || cur === 'all') ? 'din' : 'none') : cur);
    // Exclusive across every MIDI-capable track: the synth voices plus the drum track (finger-drumming)
    // when the firmware exposes it. One USB keyboard has exactly one target.
    const idxs: number[] = [];
    for (let t = 0; t < synthCount; t++) idxs.push(t);
    if (drumSrcIdx != null) idxs.push(drumSrcIdx);
    const updates: Record<number, { src: string; srcch: number }> = {};
    for (const t of idxs) {
      const cur = trkSubs[t] ?? { src: 'none', srcch: 0 };
      const ns = nextFor(t, cur.src);
      if (ns !== cur.src) updates[t] = { src: ns, srcch: cur.srcch };
    }
    setTrkSubs(m => ({ ...m, ...updates }));
    Object.keys(updates).forEach(k => tp.trk(+k, 'SRC=' + cmdForSrc(updates[+k].src)));
  };
  const usbKbd = (i: number) => <KbdBtn owned={usbOwner(i)} onPress={() => claimUsb(i)} />;
  // (The old "Synth B enable" ● On / ○ Off toggle was removed: the pool split is now PERMANENT on
  // every build that has a Synth B — the fixed 4-way split (4-voice) and the hetero build force it on
  // at boot and ignore @VOICE2, so there's nothing to enable. Synth B is always live.)

  // The folder browser (nav bar + picker), shared by the Synth and Synth/Voices 2 pages.
  // Both share the browse position (cart/folder) but keep their own selection + list ref, and
  // pick into their own voice slot. `target` routes taps to voice 1 or voice 2.
  const voiceBrowserBody = (target: number) => {
    // NON-DEXED engine (OPLL / Plaits / any future engine): show the ENGINE'S OWN fixed patch list
    // (from @TRK<i>.INSTRS), not the Dexed /dexed cart browser. Tapping a row selects that patch;
    // the current one is highlighted. Row labels strip the firmware's "OPLL: "/"Plaits: " prefix.
    const oi = target - 1;
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
    );
  };

  // ---- Per-player loop recorder ------------------------------------------------------------
  // Each MIDI player owns its synth's looper: player 1 -> voice 1 (g_loop1), player 2 -> voice 2
  // (g_loop2). Each looper taps its voice's arp downstream, so it captures THAT player's song +
  // your live keys (the combined post-arp stream) and loops it back into that synth only.
  // The firmware keeps one selected target (recSel()/@RECV), so a player's button points it at
  // its own voice first, then acts — that way the two players' record controls never collide.
  type RecDeckT = { st: number; prog: number; bars: number; setBars: (n: number) => void;
                    record: () => void; overdub: () => void; stop: () => void; clear: () => void };
  const recDeck = (v: 1 | 2): RecDeckT => {
    const st = v === 2 ? rec.st2 : rec.st1;
    const prog = v === 2 ? rec.p2 : rec.p1;
    const bars = v === 2 ? rec.bars2 : rec.bars1;
    // Optimistic local state; the device's @RECP push corrects it.
    const setSt = (s: number) => setRec(r => (v === 2 ? { ...r, v, st2: s } : { ...r, v, st1: s }));
    const aim = () => tp.recVoice(v);   // always send @RECV (idempotent, cheap) — never trust stale rec.v
    return {
      st, prog, bars,
      // Loop length is per-synth (@RECBARS targets the selected voice), so A can loop a 2-bar
      // riff under B's 8-bar pad. Takes effect on that synth's next fresh recording.
      setBars: (n: number) => { aim(); tp.recBars(n); setRec(r => (v === 2 ? { ...r, v, bars2: n } : { ...r, v, bars1: n })); },
      record:  () => { aim(); tp.recArm(true);     setSt(1); },
      overdub: () => { aim(); tp.recOverdub(true); setSt(3); },
      stop:    () => { aim(); tp.recArm(false);    setSt(st === 2 || st === 3 ? 4 : 0); },
      clear:   () => { aim(); tp.recClear();       setSt(0); },
    };
  };
  // The record row shown inside a MIDI player's page: Record / Overdub / Stop / Clear + live state.
  const recRow = (v: 1 | 2) => {
    const d = recDeck(v);
    const armed = d.st === 1, capturing = d.st === 2 || d.st === 3;
    return (
      <>
        <Row>
          <Pressable style={[s.btn, s.grow1, (armed || capturing) && s.btnRecOn]} onPress={d.record}>
            <Text style={s.btnText}>●  Record</Text>
          </Pressable>
          <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={d.overdub}><Text style={s.btnText}>＋  Overdub</Text></Pressable>
          <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={d.stop}><Text style={s.btnText}>■  Stop</Text></Pressable>
          <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={d.clear}><Text style={s.btnText}>✕  Clear</Text></Pressable>
        </Row>
        <Row><Text style={[s.muted, { flex: 1 }]}>Loop length</Text>
          {[1, 2, 4, 8].map(n => (
            <Pressable key={n} style={[s.pill, d.bars === n && s.pillOn]} onPress={() => d.setBars(n)}>
              <Text style={s.text}>{n} {n === 1 ? 'bar' : 'bars'}</Text>
            </Pressable>
          ))}</Row>
        <Text style={s.muted}>
          {REC_STATES[d.st]}{d.st === 0 ? ' — captures this synth’s song + your live keys, then loops it.' : ''}
          {' '}Length is this synth’s own; the time signature is the shared master meter (set it on Metronome).
        </Text>
        {d.st > 0 && <ProgressBar value={d.prog} />}
      </>
    );
  };

  // These section bodies (+ their header transport rows) are shared: each drives its own
  // standalone card/page AND its submenu entry, so there's a single source of truth for the
  // MIDI-player, Synth/Voices and Arpeggiator UIs.

  // ---- MIDI player renderers: ONE component set, driven by a deck (see makeSongDeck) --------
  // Both players render from these. Edit once, both change; a third synth is a new deck.
  const playerActions = (D: SongDeckT) => (<>
    <HdrBtn label="‹" stop onPress={() => D.step(-1)} />
    <HdrBtn label="›" stop onPress={() => D.step(1)} />
    <HdrBtn label="▶" onPress={D.play} active={D.player.playing} />
    <HdrBtn label="■" stop onPress={D.stop} />
    {!D.noEndMode && <HdrBtn label={(END_MODES.find(m => m.key === D.endMode) || END_MODES[3]).icon} stop onPress={D.cycleEnd} />}
  </>);
  // Baked test/demo songs live in flash (no `file` field — they play by NAME on the firmware's
  // non-.mid branch). Surface them as a synthetic "built-ins" folder injected at the /midi root,
  // so the browser shows every real card folder (songs, drums, loops, tests, …) PLUS the baked
  // demos. Named "built-ins" (not "tests") so it can't collide with the real /midi/tests folder
  // that the browser now lists alongside it.
  const bakedSongLeaves = cat.songs.filter(s => !s.file).map(s => ({ name: s.name, arg: s.name }));
  const songInjectFolders = bakedSongLeaves.length ? [{ name: 'built-ins', leaves: bakedSongLeaves }] : [];
  // The song half: pick a song via the recursive folder browser, set the player's level, choose
  // what happens when it ends. A tap plays the file immediately (restart on a fresh downbeat).
  const playerSongBody = (D: SongDeckT) => (
    <>
      <VolSlider label="Volume" value={D.vol} onChange={D.onVol} onCommit={D.commitVol} disabled={!connected} />
      {!!D.volNote && <Text style={s.muted}>{D.volNote}</Text>}
      <View style={s.browseBox}>
        <FolderBrowser tp={tp} root={D.browseRoot ?? '/midi'} ext="mid" enabled={connected && loaded}
          selected={D.player.song} playing={D.player.playing ? D.player.song : undefined}
          onSelectFile={(full, disp) => D.playFile(full, disp)} injectFolders={D.injectFolders ?? songInjectFolders}
          onFolderList={D.onFolderList} filesFirst={D.filesFirst} />
      </View>
      {!D.noEndMode && <Row><Text style={[s.muted, { flex: 1 }]}>When finished</Text>
        {END_MODES.map(m => (
          <Pressable key={m.key} style={[s.pill, D.endMode === m.key && s.pillOn]} onPress={() => D.applyEnd(m.key)}>
            <Text style={s.text}>{m.icon}  {m.label}</Text>
          </Pressable>
        ))}</Row>}
    </>
  );
  // The player page splits in two: the song selector, and THIS synth's own loop recorder. The
  // header transport (‹ › ▶ ■) drives the SONG on both tabs; the looper has its own Record/Stop
  // in its tab. The Looper tab only exists on a recorder build.
  const playerBody = (D: SongDeckT) => (
    <BodyTabs tabs={[
      { key: 'song', label: 'MIDI PLAYER', body: playerSongBody(D) },
      ...(caps.rec ? [{ key: 'loop', label: 'MIDI LOOPER', body: recRow(D.v) }] : []),
      ...(caps.rec && caps.recedit ? [{ key: 'edit', label: 'NOTE EDITOR', body: <PianoRoll tp={tp} voice={D.v} maxEvents={rec.max} /> }] : []),
    ]} />
  );
  // Card/page subtitle + progress bar (the bar only once the device reports a position).
  const playerValue = (D: SongDeckT) => (D.player.playing ? '♪ ' : '') + (D.player.name || '—');
  const playerProgress = (D: SongDeckT) => (D.player.playing && D.player.prog >= 0 ? D.player.prog : undefined);

  // MIDI Input selector (Phase 3, Thread C) — which physical device plays THIS synth, plus an
  // optional channel filter. `i` is the firmware track index (synth card N -> track N-1). Switching
  // is a pure @TRK<i>.SRC subscription write on the device: no repatch, no audio/loop/clock impact —
  // you can move a keyboard between synths mid-performance with no dropout. Reuses the shared pill
  // styles so it's ONE control every synth card renders (data-driven; a new voice reuses it verbatim).
  // BT/Serial inputs aren't wired as sources yet (deferred), so the picker offers the real local
  // devices: Off / DIN / USB / Both. "Both" (all local) reads back from the device as "multi".
  const midiInputBody = (i: number) => {
    const sub = trkSubs[i] || { src: 'none', srcch: 0 };
    const active = sub.src === 'multi' ? 'all' : sub.src;   // both-local reports as "multi"
    const devs = [{ k: 'none', l: 'Off' }, { k: 'din', l: 'DIN' }, { k: 'usb', l: 'USB' }, { k: 'all', l: 'Both' }];
    const setSrc = (k: string) => { setTrkSubs(m => ({ ...m, [i]: { src: k, srcch: (m[i]?.srcch ?? 0) } })); tp.trk(i, 'SRC=' + k); };
    const setCh  = (c: number) => { setTrkSubs(m => ({ ...m, [i]: { src: (m[i]?.src ?? 'none'), srcch: c } })); tp.trk(i, 'SRCCH=' + c); };
    return (
      <View style={{ marginTop: 10 }}>
        <Text style={s.muted}>MIDI Input — which device plays this synth (switches live, no dropout)</Text>
        <Row>
          {devs.map(d => (
            <Pressable key={d.k} style={[s.pill, s.grow1, active === d.k && s.pillOn]} disabled={!connected} onPress={() => setSrc(d.k)}>
              <Text style={s.text}>{d.l}</Text>
            </Pressable>
          ))}
        </Row>
        {active !== 'none' && (<>
          <Text style={[s.muted, { marginTop: 6 }]}>Channel filter</Text>
          <View style={s.patGrid}>
            {Array.from({ length: 17 }, (_, c) => (
              <Pressable key={c} style={[s.pill, sub.srcch === c && s.pillOn]} disabled={!connected} onPress={() => setCh(c)}>
                <Text style={s.text}>{c === 0 ? 'All' : String(c)}</Text>
              </Pressable>
            ))}
          </View>
        </>)}
      </View>
    );
  };

  const synthActions = (<>
    <HdrBtn label="‹ Prev" stop onPress={() => stepVoice(-1)} />
    <HdrBtn label="Next ›" stop onPress={() => stepVoice(1)} />
  </>);
  // Quick-action cluster for a synth's HOME card: step the INSTRUMENT (voice) + play/stop/loop the
  // track's MIDI player, so the common controls work without opening the card. `voiceTarget` is the
  // stepVoice slot (1 = Synth A, 2 = Synth B, v+1 for the extra voices); the deck is the same one the
  // MIDI Player page drives (so Play uses the folder-aware "play the selected track" logic).
  const synthQuick = (voiceTarget: number, deck: SongDeckT, arpOn?: boolean, toggleArp?: () => void) => (
    <>
      <HdrBtn label="‹ Ins" stop onPress={() => stepVoice(-1, voiceTarget)} />
      <HdrBtn label="Ins ›" stop onPress={() => stepVoice(1, voiceTarget)} />
      <HdrBtn label="▶" onPress={deck.play} active={deck.player.playing} />
      <HdrBtn label="■" stop onPress={deck.stop} />
      {toggleArp && <HdrBtn label="Arp" onPress={toggleArp} active={!!arpOn} />}
      <HdrBtn label="↻" active={deck.endMode === 'repeat'} onPress={() => deck.applyEnd(deck.endMode === 'repeat' ? 'stop' : 'repeat')} />
    </>
  );
  const synthBody = (
    <View style={s.synthWrap}>
      {/* Synth output level — the same control as the MIDI Player's Volume (both drive
          @SONGVOL, the synth mix-bus fader), surfaced here since this is the synth page. */}
      <VolSlider label="Volume" value={songVol} onChange={setSongVol} onCommit={v => tp.songVol(v)} disabled={!connected} />
      {voiceBrowserBody(1)}
      {midiInputBody(0)}
    </View>
  );
  // Voices-2 (keyboard-split) body/actions — shared by the standalone Synth/Voices 2 card and
  // the Synthesizer B tabbed page.
  const synth2Actions = (<>
    <HdrBtn label="‹ Prev" stop onPress={() => stepVoice(-1, 2)} />
    <HdrBtn label="Next ›" stop onPress={() => stepVoice(1, 2)} />
  </>);
  const synth2Body = (
    <View style={s.synthWrap}>
      {/* Synth B is always live now (the pool split is permanent). This page just picks the voice + level. */}
      <VolSlider label="Volume" value={voice2.vol} onChange={v => setVoice2(x => ({ ...x, vol: v }))} onCommit={v => tp.voice2Vol(v)} disabled={!connected} />
      {voiceBrowserBody(2)}
      {midiInputBody(1)}
    </View>
  );
  // Tempo / Drums / Metronome bodies + actions — shared by the standalone cards (if shown) and
  // the unified "Tempo" tabbed page (single source of truth).
  const tempoActions = (<>
    <HdrBtn label="−" stop onPress={() => stepBpm(-1)} />
    <HdrBtn label="＋" onPress={() => stepBpm(1)} />
  </>);
  const tempoBody = (
    <>
      <Text style={{ color: C.text, textAlign: 'center', fontSize: 30, fontWeight: '800' }}>{Math.round(bpm)} <Text style={{ fontSize: 15, color: C.muted }}>BPM</Text></Text>
      <View style={s.volRow}>
        <Pressable style={s.pill} onPress={() => stepBpm(-1)}><Text style={s.text}>−</Text></Pressable>
        <ThrottledSlider min={40} max={240} value={bpm} onChange={setBpm} onCommit={b => tp.masterBpm(b)} />
        <Pressable style={s.pill} onPress={() => stepBpm(1)}><Text style={s.text}>＋</Text></Pressable>
      </View>
      <Pressable style={[s.btn, s.btnGhost]} onPress={() => { const b = player.playing ? songBpm : 120; setBpm(b); tp.masterBpm(b); }}>
        <Text style={s.btnText}>Reset → {player.playing ? songBpm + ' (playing song)' : '120'} BPM</Text></Pressable>
      <Text style={s.muted}>Master tempo — the metronome clock every MIDI player, drum groove, and arp locks to. Songs play at THIS tempo (they no longer change it); set it here or on the top transport bar.</Text>
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>Launch quantize</Text>
          <Text style={s.muted}>Start songs & grooves on the next bar so they lock together.</Text>
        </View>
        <Switch value={quant} onValueChange={v => { setQuant(v); tp.launchQuantize(v); }} /></Row>
    </>
  );
  const metroActions = (<>
    <HdrBtn label="▶" onPress={playMetro} active={metro.on} />
    <HdrBtn label="■" stop onPress={stopMetro} />
  </>);
  const metroBody = (
    <>
      {/* Play/Stop here are the SAME master transport as the top bar (this is the app-wide clock);
          Mute toggles only whether you hear the click. Active state = which button is lit. */}
      <Row>
        <Pressable style={[s.btn, s.grow1, metro.on ? s.btnOn : s.btnIdle]} onPress={playMetro}>
          <Text style={s.btnText}>▶  {metro.on ? 'Running' : 'Play'}</Text>
        </Pressable>
        <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={stopMetro}>
          <Text style={s.btnText}>■  Stop</Text>
        </Pressable>
      </Row>
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>Hear the click</Text>
          <Text style={s.muted}>The transport clock runs either way — this just plays the click out the speakers. Off by default.</Text>
        </View>
        <Switch value={!metro.muted} onValueChange={v => { setMetro(m => ({ ...m, muted: !v })); tp.metronomeMute(!v); }} /></Row>
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>{metro.locked ? '🔒' : '🔓'}  Lock tempo</Text>
          <Text style={s.muted}>Off: the first song, loop, or groove you start sets the tempo to its own BPM. On: the tempo stays put — loading content no longer changes it. Off by default.</Text>
        </View>
        <Switch value={metro.locked} onValueChange={v => { setMetro(m => ({ ...m, locked: v })); tp.metronomeLock(v); }} /></Row>
      <Text style={s.muted}>Play defines the downbeat and starts the master clock — both MIDI players, the drums, and the arps lock to it. Runs at the master tempo ({Math.round(bpm)} BPM); set it on the top bar or the Tempo tab. Stop clears everything.</Text>
      <Text style={[s.text, { marginTop: 6 }]}>Click volume</Text>
      <VolSlider label="Volume" value={metro.vol} onChange={v => setMetro(m => ({ ...m, vol: v }))} onCommit={v => tp.metronomeVol(v)} disabled={!connected} />
      <Text style={[s.text, { marginTop: 6 }]}>Time signature</Text>
      <Text style={s.muted}>Beats per bar — the click accents beat 1 of each bar. The metronome runs on its own clock, so this is independent of any song or groove that's playing.</Text>
      <Row>
        {[2, 3, 4, 5, 6, 7].map(n => (
          <Pressable key={n} style={[s.pill, metro.sig === n && s.pillOn]}
            onPress={() => { setMetro(m => ({ ...m, sig: n })); tp.metronomeSig(n); }}>
            <Text style={s.text}>{n}/4</Text>
          </Pressable>
        ))}
      </Row>
    </>
  );
  // REVERB — the FX card (caps.fx). One reverb per build: @STATE.fx.type picks the character sliders
  // (plate = size/damp/diffuse/shimmer/freeze; spring = time/treble/bass). ROUTE (@STATE.fx.route)
  // picks the topology: 'insert' = classic master reverb (Enable + Dry/Wet + character); 'send' = a
  // per-voice aux SEND bus (Enable + a send slider per track + a wet Return + character). Each control
  // sends @FX.<PARAM> / @TRK<i>.FXSEND on release (throttle-friendly); state hydrates from @STATE.
  //
  // Shared character params — identical in both topologies (they shape the reverb, not the routing).
  const fxCharacter = fx.type === 'spring' ? (
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
  // One send-matrix row: a throttled slider (0..100) tapping a track into the reverb bus.
  const fxSendRow = (key: string, label: string, value: number, onChange: (v: number) => void, onCommit: (v: number) => void) => (
    <VolSlider key={key} label={label} value={value} max={100} onChange={onChange} onCommit={onCommit} disabled={!connected || !fx.on} />
  );
  const reverbBody = fx.route === 'send' ? (
    <>
      <Row><View style={{ flex: 1 }}>
          <Text style={s.text}>Enable reverb</Text>
          <Text style={s.muted}>Turns the {fx.type === 'spring' ? 'spring' : 'plate'} reverb bus on. Raise a track's send to reverb it; sends at 0 stay dry.</Text>
        </View>
        <Switch value={fx.on} onValueChange={v => { setFx(f => ({ ...f, on: v })); tp.fx('ON=' + (v ? 1 : 0)); }} /></Row>
      <Text style={[s.text, { marginTop: 8 }]}>Sends</Text>
      <Text style={s.muted}>How much of each track feeds the reverb. 0 = fully dry.</Text>
      {Array.from({ length: synthCount }).map((_, v) =>
        fxSendRow('snd' + v, 'Synth ' + String.fromCharCode(65 + v) + (trkNames[v] ? ' · ' + engName(trkNames[v], trkEng[v]) : ''),
          trkSend[v] ?? 0, val => setTrkSend(m => ({ ...m, [v]: val })), val => tp.trk(v, 'FXSEND=' + val)))}
      {(cat.hasDrums || drumLive) &&
        fxSendRow('snddrum', 'Drums', trkSend[synthCount] ?? 0, val => setTrkSend(m => ({ ...m, [synthCount]: val })), val => tp.trk(synthCount, 'FXSEND=' + val))}
      {caps.audioloop > 0 &&
        fxSendRow('sndloop', 'Audio Loop', fx.loopsend, val => setFx(f => ({ ...f, loopsend: val })), val => tp.fx('LOOPSEND=' + val))}
      <Text style={[s.text, { marginTop: 10 }]}>Return</Text>
      <VolSlider label="Wet" value={fx.return} max={100} onChange={v => setFx(f => ({ ...f, return: v }))} onCommit={v => tp.fx('RETURN=' + v)} disabled={!connected || !fx.on} />
      {fxCharacter}
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
      {fxCharacter}
    </>
  );
  // The Drums card's "Loops" page + header transport REUSE the synth deck component: playerSongBody
  // (drumDeck) / playerActions(drumDeck) / playerValue(drumDeck). No bespoke drum player — a drum
  // track is just another synth (see drumDeck above). Only the KIT page is drum-specific:
  // What's actually generating the drums: the engine kind (@STATE catalog drumEngine — e.g. "OPLL"
  // rhythm voice, or "TSF" sampled kit) and, when the engine has selectable GM kits, the loaded kit.
  // Shown on the Drum Loops page + tile so it's clear what the grooves are playing through.
  // cat.drumEngine = a SEPARATE drum voice (OPLL rhythm / TSF). When it's empty, the primary synth
  // renders its own ch10 drums, so the drum engine IS the primary engine — name it (cat.engine, e.g.
  // "OPLL (YM2413)" -> "OPLL") instead of a generic "GM".
  const drumEngineLabel = cat.drumEngine ? cat.drumEngine + ' drum engine'
    : cat.hasDrums ? (cat.engine ? cat.engine.split(/[\s(]/)[0] : 'GM') + ' drums'
    : 'Drums';
  // SD-sampler kits are /drums folder names ("808_clean"); prettify to "808 Clean". Names without an
  // underscore (GM/acoustic kit display names) are already human-readable, so leave them untouched.
  const prettyKit = (s: string) => s && s.includes('_') ? s.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase()) : s;
  const drumKitName = prettyKit(cat.drumkits[drums.kit]?.name || '');
  // ‹ Prev / Next › through the kit list (wraps). Same @DRUMKIT the picker sends; the instrument
  // sub-card's `actions` render these on BOTH the Kit card and its detail page.
  const cycleKit = (dir: number) => {
    const n = cat.drumkits.length;
    if (!n) return;
    const k = (((drums.kit || 0) + dir) % n + n) % n;
    setDrums(d => ({ ...d, kit: k }));
    tp.drumKit(k);
  };
  const kitNav = caps.drumkitsel && cat.drumkits.length > 1
    ? <><HdrBtn label="‹ Prev" stop onPress={() => cycleKit(-1)} /><HdrBtn label="Next ›" stop onPress={() => cycleKit(1)} /></>
    : undefined;
  // A fixed-voice engine (OPLL rhythm, caps.drumkitsel === false) has no GM kits — say so instead of
  // showing a kit name the engine ignores. GM/sampled engines append the loaded kit.
  const drumDetail = !caps.drumkitsel ? drumEngineLabel + '  ·  fixed rhythm voice'
    : drumKitName ? drumEngineLabel + '  ·  Kit: ' + drumKitName
    : drumEngineLabel;
  // KIT page — mirrors the synth's Synth / Voices: pick the GM drum kit (the drum "instrument") + level.
  const drumKitBody = (
    <>
      <VolSlider label="Volume" value={drumVol} onChange={setDrumVol} onCommit={v => tp.drumVol(v)} disabled={!connected} />
      {caps.drumfontsel && fonts.length > 0 && (
        // Drum Font picker (runtime @DRUMFONT swap): switch which SF2 is resident. A picker, not a
        // layer — one font is loaded at a time. Highlight the resident one (@STATE.drumfont.path,
        // falling back to the list's current-flag). Selecting one swaps live; the kit list below
        // repopulates once the device acks (@DRUMFONT= → catalog reload).
        <>
          <Text style={[s.muted, { marginTop: 8 }]}>Drum Font: {drumFont.display || fonts.find(f => f.current)?.display || '—'}</Text>
          <Row><ScrollView horizontal showsHorizontalScrollIndicator={false}>
              {fonts.map((f, i) => {
                const on = drumFont.path ? f.path === drumFont.path : f.current;
                return <Pressable key={i} style={[s.pill, on && s.pillOn]} disabled={!connected} onPress={() => { setDrumFont({ path: f.path, display: f.display }); tp.drumFont(f.path); }}><Text style={s.text}>{f.display}</Text></Pressable>;
              })}
          </ScrollView></Row>
        </>
      )}
      {caps.drumkitsel ? (
        <>
          <Text style={[s.muted, { marginTop: 8 }]}>Kit: {drumKitName || '—'}</Text>
          {/* Vertical scrollable kit list (the Mars library is ~90 kits — a horizontal pill row hid the
              tail). Mirrors the Voices FlatList: fixed ROW_H rows, bounded height so it scrolls in-card. */}
          <View style={{ height: Math.min(Math.max(cat.drumkits.length, 1), 7) * ROW_H, borderWidth: 1, borderColor: C.border, borderRadius: 7, marginTop: 4 }}>
            <FlatList data={cat.drumkits} nestedScrollEnabled keyExtractor={(_, i) => 'dk' + i}
              getItemLayout={(_, index) => ({ length: ROW_H, offset: ROW_H * index, index })}
              renderItem={({ item, index }) => <ListBtn label={prettyKit(item.name)} sel={drums.kit === index} onPress={() => { setDrums(d => ({ ...d, kit: index })); tp.drumKit(index); }} />} />
          </View>
        </>
      ) : (
        // Fixed-voice drum engine (OPLL rhythm): no GM kits to pick — say what it actually is so the
        // page isn't a list of switches that do nothing (caps.drumkitsel === false from the firmware).
        <>
          <Text style={[s.text, { marginTop: 8 }]}>{(cat.drumEngine || 'OPLL')} rhythm voice</Text>
          <Text style={s.muted}>A fixed 5-sound rhythm set — Bass Drum, Snare, Tom, Top Cymbal, Hi-Hat. There are no swappable GM kits on this engine; grooves play straight through these sounds. (Flash a TSF drum-font build for selectable kits.)</Text>
        </>
      )}
    </>
  );

  // ---- Audio loop controls (see planning/audio-looper/DESIGN.md) --------------
  // N independent audio loops record the MASTER MIX (not MIDI). aloop.sel is the loop the
  // buttons act on. Recording starts on the next bar downbeat (no "play a note" wait —
  // audio has no note-on), so the armed label differs from the MIDI recorder's.
  const AL_STATES = ['Empty', 'Armed — starts on the next bar', 'Recording', 'Overdubbing', 'Looping'];
  const alSt = () => aloop.st[aloop.sel] ?? 0;
  const alProg = () => aloop.p[aloop.sel] ?? 0;
  const setAlSt = (st: number) => setAloop(a => ({ ...a, st: a.st.map((v, i) => (i === a.sel ? st : v)) }));
  const setAlSel = (i: number) => { setAloop(a => ({ ...a, sel: i })); tp.audioLoopSel(i); };
  const setAlBars = (n: number) => { setAloop(a => ({ ...a, bars: n })); tp.audioLoopBars(n); };
  const setAlMono = (on: boolean) => { setAloop(a => ({ ...a, mono: on })); tp.audioLoopMono(on); };
  const setAlFollow = (on: boolean) => { setAloop(a => ({ ...a, follow: on })); tp.audioLoopFollow(on); };
  const alRecord = () => { tp.audioLoopArm(true); setAlSt(1); };
  const alOverdub = () => { tp.audioLoopOverdub(true); setAlSt(3); };
  const alStopBtn = () => { const st = alSt(); tp.audioLoopArm(false); setAlSt(st === 2 || st === 3 ? 4 : 0); };
  const alClearBtn = () => { tp.audioLoopClear(); setAlSt(0); };
  const alSaveBtn = () => tp.audioLoopSave('loop' + (aloop.sel + 1));
  // How long `bars` will be at the current tempo/meter — used to grey out lengths that
  // exceed this loop's buffer (the no-PSRAM board only fits ~1 s).
  const alBarsSecs = (n: number) => (n * metro.sig * 60) / Math.max(1, bpm);
  const alFits = (n: number) => aloop.capS <= 0 || alBarsSecs(n) <= aloop.capS + 0.01;

  // NOTE: the standalone "Loop Recorder" card is GONE. The MIDI looper now belongs to each
  // synth's MIDI player (see recDeck/recRow above) — one recorder per synth, with its own loop
  // length — and the audio loop recorder owns the top-level Audio Loop card. So there's no
  // shared voice-selector/bars UI here any more; every rec control is per-player.

  // Extra synth voices (Phase 3, data-driven): voices 2+ of a 4-voice pool get a GENERATED card
  // each — a FULL peer of Synth A/B with the same submenu (Synth/Voices + MIDI Player + Arpeggiator),
  // reusing the SAME components (voice browser, makeSongDeck, arpBody) driven by the uniform @TRK<i>.*
  // wire. No hand-instantiation, so more firmware voices grow the UI with zero edits. Rendered only
  // when @STATE tracks[] reports that many synth voices (synthCount). Voices 0/1 keep their bespoke
  // cards (the Synth-B split toggle / shared voice-2 bus that don't apply to the fixed N-way pool).
  const DEF_ARP = { on: false, pat: 0, rate: 0, oct: 1, latch: false };
  // ── makeTrackCard: the ONE per-track card+detail component ────────────────────────────────────
  // A track (a synth voice OR the drum track) renders as a parent submenu card + its child pages.
  // This factory owns that STRUCTURE — the stable child ids (parentId+'p'/'v'/'a'), the parent↔child
  // linkage, the SubMenu wiring, and the shared player renderers — while each caller supplies the
  // CONTENT (bodies, labels, actions) for its engine kind. So the drum track is LITERALLY the same
  // component as a synth voice, just fed a groove browser + Kit page instead of a MIDI player + voice
  // browser (the two genuine divergences, passed in as `player.body` / `instrument`). Returns the
  // parent card and its children separately so callers slot them into the two section groups.
  type TrackCardCfg = {
    parentId: string; title: string; theme: { accent: string; tint: string };
    show: boolean; disabledReason?: string; deck: SongDeckT;
    landing: { value: string; subtitle?: React.ReactNode; topRight?: React.ReactNode; actions?: React.ReactNode;
               progress?: number; foot?: React.ReactNode; bodyPrefix?: React.ReactNode };
    player: { title: string; fullHeight?: boolean; body: React.ReactNode };
    instrument: { title: string; value: string; subtitle?: React.ReactNode; topRight?: React.ReactNode;
                  actions?: React.ReactNode; body: React.ReactNode; fullHeight?: boolean };
    arp?: ArpSlotT;
  };
  const makeTrackCard = (c: TrackCardCfg): { parent: Section; children: Section[] } => {
    const { parentId, theme, deck } = c;
    const submenu = <SubMenu getItems={() => sections.filter(x => x.parent === parentId).sort((a, b) => ord(a.id) - ord(b.id))} onOpen={setRoute} accent={theme.accent} tint={theme.tint} />;
    const parent: Section = {
      id: parentId, title: c.title, show: c.show, asCard: true, accent: theme.accent, tint: theme.tint,
      disabledReason: c.disabledReason, value: c.landing.value, subtitle: c.landing.subtitle,
      topRight: c.landing.topRight, actions: c.landing.actions, progress: c.landing.progress, foot: c.landing.foot,
      body: c.landing.bodyPrefix ? <View style={{ gap: 10 }}>{c.landing.bodyPrefix}{submenu}</View> : submenu,
    };
    const children: Section[] = [
      { id: parentId + 'p', title: c.player.title, show: false, parent: parentId, fullHeight: c.player.fullHeight, accent: theme.accent, tint: theme.tint,
        value: playerValue(deck), progress: playerProgress(deck), actions: playerActions(deck), body: c.player.body },
      { id: parentId + 'v', title: c.instrument.title, show: false, parent: parentId, fullHeight: c.instrument.fullHeight, accent: theme.accent, tint: theme.tint,
        value: c.instrument.value, subtitle: c.instrument.subtitle, topRight: c.instrument.topRight, actions: c.instrument.actions, body: c.instrument.body },
    ];
    if (c.arp) children.push({ id: parentId + 'a', title: 'Arpeggiator', show: false, parent: parentId, accent: theme.accent, tint: theme.tint,
      value: arpValue(c.arp), actions: arpActions(c.arp), body: arpBody(c.arp) });
    return { parent, children };
  };
  const extraSynthCards: Section[] = [];
  const extraChildCards: Section[] = [];
  for (let v = 2; v < synthCount; v++) {
    const theme = v === 2 ? THEME.synthC : THEME.synthD;
    const letter = String.fromCharCode(65 + v);   // C, D, …
    const parentId = 'synthX' + v;
    const arpState = arpX[v] ?? DEF_ARP;
    const mstop = (manualStopRefX.current[v] ??= { current: false });
    // MIDI-player deck for this voice — the drumDeck pattern (a view over playerX[v]) wired to @TRK<i>.
    const deck = makeSongDeck({
      v: 1, persistKey: 'end', manualStopRef: mstop, deckKey: 'x' + v,
      player: playerX[v] ?? { song: '', playing: false, name: '', prog: 0 },
      setPlayer: upd => setPlayerX(m => { const cur = m[v] ?? { song: '', playing: false, name: '', prog: 0 }; return { ...m, [v]: typeof upd === 'function' ? (upd as any)(cur) : upd }; }),
      endMode: endModeX[v] ?? 'stop', setEndMode: em => setEndModeX(m => ({ ...m, [v]: em })),
      vol: trkVolX[v] ?? 100, onVol: n => setTrkVolX(m => ({ ...m, [v]: n })), commitVol: n => tp.trk(v, 'VOL=' + n),
      wire: { play: a => tp.trk(v, 'PLAY=' + a), restart: a => tp.trk(v, 'RESTART=' + a), stop: () => tp.trk(v, 'STOP'), loop: on => tp.trk(v, 'LOOP=' + (on ? 1 : 0)) },
    });
    // Arp slot for this voice — same ArpSlotT the A/B arps use, targeting @TRK<i>.ARP*.
    const setA = (patch: Partial<typeof DEF_ARP>) => setArpX(m => ({ ...m, [v]: { ...(m[v] ?? DEF_ARP), ...patch } }));
    const setPid = (id: string) => setArpPresetIdX(m => ({ ...m, [v]: id }));
    const vSeq = seqX[v] ?? DEFAULT_SHAPE.steps;
    const selectPat = (i: number) => { setA({ pat: i }); tp.trk(v, 'ARPPAT=' + i); if (i === PAT_USER_SEQUENCE) tp.trk(v, 'ARPSEQ=' + encodeSequence(vSeq)); setPid(''); };
    const arpSlot: ArpSlotT = {
      arp: arpState, mode: arpModeX[v] ?? 'preset', setMode: m => setArpModeX(x => ({ ...x, [v]: m })),
      presetId: arpPresetIdX[v] ?? '', activeName: ARP_LIBRARY.find(p => p.id === (arpPresetIdX[v] ?? ''))?.name || '', seq: vSeq,
      play: () => { if (arpState.on) tp.trk(v, 'ARPRESTART'); else { setA({ on: true }); tp.trk(v, 'ARPON=1'); } },
      stop: () => { setA({ on: false }); tp.trk(v, 'ARPON=0'); },
      stepNav: dir => { const idx = ARP_LIBRARY.findIndex(p => p.id === (arpPresetIdX[v] ?? '')); if (!ARP_LIBRARY.length) return; const ni = (idx < 0 ? (dir > 0 ? -1 : 0) : idx) + dir; const p = ARP_LIBRARY[(ni + ARP_LIBRARY.length) % ARP_LIBRARY.length]; const st = applyArpPreset(tp, p, v + 1, arpState.latch); setA({ pat: st.pat, rate: st.rate, oct: st.oct, latch: st.latch }); if (st.seq) setSeqX(m => ({ ...m, [v]: st.seq! })); setPid(p.id); },
      selectPattern: selectPat, applySeq: st => { setSeqX(m => ({ ...m, [v]: st })); tp.trk(v, 'ARPSEQ=' + encodeSequence(st)); if (arpState.pat !== PAT_USER_SEQUENCE) { setA({ pat: PAT_USER_SEQUENCE }); tp.trk(v, 'ARPPAT=' + PAT_USER_SEQUENCE); } setPid(''); },
      applyPreset: p => { const st = applyArpPreset(tp, p, v + 1, arpState.latch); setA({ pat: st.pat, rate: st.rate, oct: st.oct, latch: st.latch }); if (st.seq) setSeqX(m => ({ ...m, [v]: st.seq! })); setPid(p.id); },
      enterManual: () => { setArpModeX(x => ({ ...x, [v]: 'manual' })); tp.trk(v, 'ARPPRESET=' + encodeArpParams({ pat: arpState.pat, rate: ARP_RATES[arpState.rate].fw, gatePct: 50, swingPct: 50, oct: arpState.oct, octMode: 0, latch: arpState.latch, velMode: 0, velFixed: 100, velAccent: 127, stepMask: -1, stepLength: 16, mpeMode: 0, outCh: 1, scatterBase: 2, scatterCount: 4, scale: 0, scaleRoot: 0, transpose: 0, repeat: 1 })); setPid(''); },
      resetManual: () => { tp.trk(v, 'ARPPRESET=' + encodeArpParams({ pat: arpState.pat, rate: ARP_RATES[arpState.rate].fw, gatePct: 50, swingPct: 50, oct: arpState.oct, octMode: 0, latch: arpState.latch, velMode: 0, velFixed: 100, velAccent: 127, stepMask: -1, stepLength: 16, mpeMode: 0, outCh: 1, scatterBase: 2, scatterCount: 4, scale: 0, scaleRoot: 0, transpose: 0, repeat: 1 })); setPid(''); },
      setRate: i => { setA({ rate: i }); tp.trk(v, 'ARPRATE=' + ARP_RATES[i].fw); setPid(''); },
      setOct: n => { setA({ oct: n }); tp.trk(v, 'ARPOCT=' + n); setPid(''); },
      setLatch: b => { setA({ latch: b }); tp.trk(v, 'ARPLATCH=' + (b ? 1 : 0)); setPid(''); },
    };
    // A generated synth voice = one makeTrackCard: MIDI Player + Synth/Voices + Arpeggiator children,
    // same structure the drum track uses. Bespoke bits (voice browser, USB-kbd claim, arp) are passed in.
    const synthTrackVol = <VolSlider label="Volume" value={trkVolX[v] ?? 100} disabled={!connected} onChange={n => setTrkVolX(m => ({ ...m, [v]: n }))} onCommit={n => tp.trk(v, 'VOL=' + n)} />;
    const engLabel = trkNames[v] ? engName(trkNames[v], trkEng[v]) : 'None';
    const { parent, children } = makeTrackCard({
      parentId, title: 'Synthesizer ' + letter, theme, show: synthCount > v, deck,
      landing: {
        value: engLabel,
        topRight: usbKbd(v),   // per-track USB-keyboard claim (white = this synth hears the USB MIDI keyboard)
        actions: synthQuick(v + 1, deck, arpState.on, () => { const on = !arpState.on; setA({ on }); tp.trk(v, 'ARPON=' + (on ? 1 : 0)); }),
        foot: <VolSlider label="Vol" value={trkVolX[v] ?? 100} onChange={n => setTrkVolX(m => ({ ...m, [v]: n }))} onCommit={n => tp.trk(v, 'VOL=' + n)} disabled={!connected} />,
        subtitle: synthCardSub(v, trkNames[v], undefined, playerX[v]?.name),
      },
      player: { title: 'MIDI Player', body: playerBody(deck) },
      instrument: {
        title: 'Synth / Voices', fullHeight: true, value: engLabel, topRight: usbKbd(v),
        actions: <><HdrBtn label="‹ Prev" stop onPress={() => stepVoice(-1, v + 1)} /><HdrBtn label="Next ›" stop onPress={() => stepVoice(1, v + 1)} /></>,
        body: <View style={s.synthWrap}>{synthTrackVol}{voiceBrowserBody(v + 1)}{midiInputBody(v)}</View>,
      },
      arp: arpSlot,
    });
    extraSynthCards.push(parent);
    extraChildCards.push(...children);
  }

  // Runtime pool partition (4-voice pool builds): redistribute the 8 Dexed engines among the 4 voices
  // live (no reflash). Fewer voices = more polyphony per voice. Sends @POOL via tp.poolPreset.
  const POOL_PRESETS = [
    { p: 0, label: '4 voices', sub: '2 engines each · 2-note MPE / 4-note poly' },
    { p: 1, label: '2 voices', sub: '4 engines each · 4-note MPE / 8-note poly' },
    { p: 2, label: '1 voice',  sub: '8 engines · 8-note MPE / 16-note poly' },
    { p: 3, label: '4 + 2 + 2', sub: 'Synth A = 4 engines; C & D = 2; B off' },
  ];
  const poolValue = ['4 voices', '2 voices', '1 voice', '4+2+2'][pool.preset] || '4 voices';
  const poolBody = (
    <View style={s.synthWrap}>
      <Text style={s.muted}>How the 8 Dexed engines are shared. Fewer voices = more notes per voice. Changes live — no reflash. (Voices with no engines are shown "off".)</Text>
      {POOL_PRESETS.map(pr => (
        <Pressable key={pr.p} onPress={() => { setPool(v => ({ ...v, preset: pr.p })); tp.poolPreset(pr.p); }}
          style={[s.listBtn, pool.preset === pr.p && s.listBtnSel]} disabled={!connected}
          accessibilityLabel={pr.label + ' — ' + pr.sub}>
          <Text style={s.text}>{(pool.preset === pr.p ? '● ' : '○ ') + pr.label}</Text>
          <Text style={s.muted}>{pr.sub}</Text>
        </Pressable>
      ))}
    </View>
  );

  // Per-track FX child page (send-matrix builds only): a card on each track's submenu (synths A-D AND
  // Drums) listing the effects fed FROM that track — today the reverb send (+ the shared bus's on/
  // return status); future effects add rows here. Gated as a whole: an FX-less build or an insert-mode
  // reverb (applied to the whole master, not per-track) yields an empty array, so no FX card appears.
  // parent/theme match each track's own card; id ends in 'f' (ord 999 -> sorts last, after Arp / Kit).
  // Shares the same trkSend state + @TRK<i>.FXSEND wire as the Reverb card's send matrix.
  const synthFxTheme = (v: number) => (v === 0 ? THEME.synthA : v === 1 ? THEME.synthB : v === 2 ? THEME.synthC : THEME.synthD);
  const synthFxParent = (v: number) => (v === 0 ? 'synthesizer' : v === 1 ? 'synthesizerB' : 'synthX' + v);
  const makeFxCard = (trkIdx: number, parentId: string, th: { accent: string; tint: string }): Section => {
    const snd = trkSend[trkIdx] ?? 0;
    const revName = fx.type === 'spring' ? 'Spring Reverb' : 'Plate Reverb';
    return {
      id: parentId + 'f', title: 'FX', show: false, parent: parentId, accent: th.accent, tint: th.tint,
      value: snd > 0 ? 'Send ' + snd + '%' : 'Dry',
      subtitle: revName,
      // Inline on the tile: the reverb SEND for this track, so you can dial it without opening the
      // sub-page (matches the Reverb card's own quick-slider foot). The full effect detail is inside.
      foot: <VolSlider label="Send" value={snd} max={100}
        onChange={n => setTrkSend(m => ({ ...m, [trkIdx]: n }))}
        onCommit={n => tp.trk(trkIdx, 'FXSEND=' + n)} disabled={!connected || !fx.on} />,
      body: (
        <View style={s.synthWrap}>
          <Text style={s.muted}>Effects fed from this track. Each send sets how much of this track goes to the shared effect — 0 keeps it dry. Character &amp; return live on the Reverb card.</Text>
          <Row><View style={{ flex: 1 }}>
              <Text style={s.text}>{revName}</Text>
              <Text style={s.muted}>{fx.on ? 'Bus on · wet return ' + fx.return + '%' : 'Bus off — enable it below or on the Reverb card'}</Text>
            </View>
            <Switch value={fx.on} onValueChange={on => { setFx(f => ({ ...f, on })); tp.fx('ON=' + (on ? 1 : 0)); }} /></Row>
          <VolSlider label="Send" value={snd} max={100}
            onChange={n => setTrkSend(m => ({ ...m, [trkIdx]: n }))}
            onCommit={n => tp.trk(trkIdx, 'FXSEND=' + n)} disabled={!connected || !fx.on} />
        </View>
      ),
    } as Section;
  };
  const fxCards: Section[] = (caps.fx && fx.route === 'send')
    ? [
        ...Array.from({ length: synthCount }).map((_, v) => makeFxCard(v, synthFxParent(v), synthFxTheme(v))),
        makeFxCard(synthCount, 'drumtrack', THEME.drums),   // Drums track (index kSynthVoices == synthCount)
      ]
    : [];

  // The drum track is the SAME component as a synth voice — one makeTrackCard. Its two genuine
  // divergences: the player child browses grooves (playerSongBody over drumDeck's /midi/drums root)
  // and the "instrument" child is the GM Kit picker (drumKitBody) instead of a voice browser; it also
  // has no Arpeggiator child. The landing shows the groove transport (tap ▶ plays a groove) + Volume.
  const drumCard = makeTrackCard({
    parentId: 'drumtrack', title: 'Drums', theme: THEME.drums,
    show: cat.hasDrums || drumLive || !!unavail.drums, disabledReason: unavail.drums, deck: drumDeck,
    landing: {
      value: playerValue(drumDeck), progress: playerProgress(drumDeck), actions: playerActions(drumDeck),
      subtitle: drumEngineLabel,   // show WHICH drum engine (OPLL/TSF/GM) plays these grooves, like the synth cards name their engine
      topRight: drumSrcIdx != null ? usbKbd(drumSrcIdx) : undefined,   // finger-drum the kit from the USB keyboard
      foot: <VolSlider label="Vol" value={drumVol} onChange={setDrumVol} onCommit={v => tp.drumVol(v)} disabled={!connected} />,
    },
    player: { title: 'Drum Loops', fullHeight: true, body: playerSongBody(drumDeck) },
    instrument: {
      title: caps.drumkitsel ? 'Kit' : 'Drum Voice', subtitle: drumDetail,
      value: caps.drumkitsel ? (cat.drumkits[drums.kit]?.name || '—') : (cat.drumEngine || 'OPLL') + ' rhythm',
      actions: kitNav,   // ‹ Prev / Next › kit — renders on both the Kit card and its detail page
      body: drumKitBody,
    },
  });

  const sections: Section[] = [
    ...extraSynthCards,
    ...extraChildCards,
    ...fxCards,
    ...drumCard.children,
    // CONNECTION — catalog stats. A Settings sub-page (reached from the Settings submenu).
    {
      id: 'conn', title: 'Connection', show: false, parent: 'settings', status: cat.engine || 'connected',
      body: (
        <>
          <Text style={s.muted}>Synth: <Text style={s.text}>{cat.engine || '—'}</Text>   ·   Transport: {TP_LABEL[tp.name]}{tp.name === 'WIFI' ? ` (${wifiHost.trim() || 'tdsp.local'})` : ''}</Text>
          {!loaded && <Text style={s.muted}>{connected ? 'Loading catalog…' : 'Connect to load the catalog.'}</Text>}
          {loaded && (
            <View style={s.statGrid}>
              <Stat label="Instruments" n={cat.instruments.length} sub="+ SD library" />
              <Stat label="Grooves" n={drumSongs.length} />
              <Stat label="Songs" n={cat.songs.length} />
              {cat.hasSf && <Stat label="Soundfonts" n={cat.soundfonts.length} />}
              <Stat label="Drum kits" n={cat.drumkits.length} />
            </View>
          )}
          <Pressable style={[s.btn, s.btnWide]} onPress={reindex} disabled={!connected || busy}>
            {busy ? <ActivityIndicator color={C.text} /> : <Text style={s.btnText}>Rebuild catalog (@REINDEX)</Text>}
          </Pressable>
        </>
      ),
    },
    // BLUETOOTH
    {
      id: 'bt', title: 'Bluetooth', show: cat.hasBt, accent: THEME.bt.accent, tint: THEME.bt.tint, status: bt.conn ? 'connected' + (bt.peer ? ': ' + bt.peer : '') : 'off',
      body: (
        <>
          <Text style={s.muted}>{bt.conn ? 'Connected: ' + (bt.peer || 'source') : 'No audio source connected'}</Text>
          <Pressable style={[s.btn, s.btnWide]} onPress={() => (bt.conn ? tp.espDisconnect() : tp.espReconnect())}>
            <Text style={s.btnText}>{bt.conn ? 'Disconnect Bluetooth Audio' : 'Connect Bluetooth Audio'}</Text>
          </Pressable>
          <Row>
            <Pressable style={s.btn} onPress={() => tp.espPair()}><Text style={s.btnText}>Pairing mode</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost]} onPress={() => tp.espForget()}><Text style={s.btnText}>Forget</Text></Pressable>
          </Row>
          <Pressable style={[s.btn, s.btnGhost, s.btnWide]} onPress={userDisconnect}>
            <Text style={s.btnText}>Disconnect App</Text>
          </Pressable>
        </>
      ),
    },
    // UNIFIED TEMPO — a submenu grouping Tempo, Drums, and Metronome. Its page lists them as the
    // original cards (keeping their quick-action shortcuts); tapping one opens that child's own
    // existing page (Back returns here). The children are gated the same as before: Drums on a
    // drum-capable build, Metronome on a @METRO-capable build.
    {
      id: 'tempo', title: 'Tempo', show: true, value: Math.round(bpm) + ' BPM', accent: THEME.tempo.accent, tint: THEME.tempo.tint,
      body: <SubMenu getItems={() => sections.filter(x => x.parent === 'tempo').sort((a, b) => ord(a.id) - ord(b.id))} onOpen={setRoute} accent={THEME.tempo.accent} tint={THEME.tempo.tint} />,
    },
    // TEMPO / BPM — master tempo; song + drums lock to it (@BPM=). A Tempo sub-page.
    {
      id: 'bpm', title: 'Tempo', show: false, parent: 'tempo', value: Math.round(bpm) + ' BPM',
      actions: tempoActions,
      body: tempoBody,
    },
    // METRONOME — on-beat click locked to the master clock. A Tempo sub-page (metro.cap builds).
    {
      id: 'metro', title: 'Metronome', show: false, parent: metro.cap ? 'tempo' : undefined, value: metro.on ? 'Playing' : 'Paused',
      actions: metroActions,
      body: metroBody,
    },
    // REVERB — FX master insert (caps.fx). Top-level card; sliders adapt to the built reverb type.
    {
      id: 'reverb', title: 'Reverb', show: caps.fx, value: fx.on ? 'On' : 'Off',
      subtitle: (fx.type === 'spring' ? 'Spring' : 'Plate') + (fx.route === 'send' ? ' · Send bus' : ''),
      // On the tile: quick On/Off + the wet level (Return in send mode, Dry/Wet Mix in insert mode),
      // so you can dial reverb without opening the card. Per-track sends live inside the card.
      foot: (
        <>
          <Row><Text style={[s.text, { flex: 1 }]}>{fx.on ? 'On' : 'Off'}</Text>
            <Switch value={fx.on} onValueChange={v => { setFx(f => ({ ...f, on: v })); tp.fx('ON=' + (v ? 1 : 0)); }} /></Row>
          {fx.route === 'send'
            ? <VolSlider label="Return" value={fx.return} max={100} onChange={v => setFx(f => ({ ...f, return: v }))} onCommit={v => tp.fx('RETURN=' + v)} disabled={!connected || !fx.on} />
            : <VolSlider label="Mix" value={fx.mix} max={100} onChange={v => setFx(f => ({ ...f, mix: v }))} onCommit={v => tp.fx('MIX=' + v)} disabled={!connected || !fx.on} />}
        </>
      ),
      body: reverbBody,
    },
    // SYNTH / VOICES — folder browser over bundled voices + the whole /dexed library
    {
      id: 'synth', title: 'Synth / Voices', show: false, parent: 'synthesizer', accent: THEME.synthA.accent, tint: THEME.synthA.tint, value: synthValue, subtitle: synthSubtitle, fullHeight: true,   // Synthesizer A sub-page
      topRight: usbKbd(0),
      actions: synthActions,
      body: synthBody,
    },
    // SYNTH / VOICES 2 — build-flag gated (caps.voice2). A full clone of the Synth/Voices
    // page (same folder browser) plus an on/off split toggle: engines 0-3 keep voice 1
    // (song/arp/drums), engines 4-7 are this voice, played live by a USB-host keyboard.
    {
      id: 'synth2', title: 'Synth / Voices 2', show: false, parent: 'synthesizerB', fullHeight: true, accent: THEME.synthB.accent, tint: THEME.synthB.tint,   // Synthesizer B sub-page
      topRight: usbKbd(1),
      value: (voice2.name ? engName(voice2.name) : 'None'),
      subtitle: synthCardSub(1, voice2.name, voice2.path, player2.name),
      actions: synth2Actions,
      body: synth2Body,
    },
    // SYNTHESIZER A — a submenu grouping the main voice-1 side: MIDI Player, Synth/Voices, and
    // Arpeggiator. Its page lists them as the original cards (keeping their quick-action shortcuts);
    // tapping one opens that child's own existing page (Back returns here).
    {
      id: 'synthesizer', title: 'Synthesizer A', show: true, value: synthValue, subtitle: synthCardSub(0, selVoiceName, selVoicePath, player.name), asCard: true,
      accent: THEME.synthA.accent, tint: THEME.synthA.tint,
      topRight: usbKbd(0),   // keyboard-ownership icon: tap to route the USB MIDI controller to this (voice-0) synth
      actions: synthQuick(1, songDeck1, arp.on, () => { const on = !arp.on; setArp(a => ({ ...a, on })); tp.arpOn(on); }),
      foot: <VolSlider label="Vol" value={songVol} onChange={setSongVol} onCommit={v => tp.songVol(v)} disabled={!connected} />,
      body: <SubMenu getItems={() => sections.filter(x => x.parent === 'synthesizer').sort((a, b) => ord(a.id) - ord(b.id))} onOpen={setRoute} accent={THEME.synthA.accent} tint={THEME.synthA.tint} />,
    },
    // SYNTHESIZER B — the keyboard-split voice-2 side as a submenu: Synth/Voices 2 and Arpeggiator 2.
    // No MIDI (single song player). Card shows on caps.voice2; the Arpeggiator 2 child only joins the
    // submenu when caps.arp2 is compiled in (its parent is set conditionally below).
    {
      id: 'synthesizerB', title: 'Synthesizer B', show: caps.voice2, asCard: true, accent: THEME.synthB.accent, tint: THEME.synthB.tint,
      topRight: usbKbd(1),
      value: (voice2.name ? engName(voice2.name) : 'None'),
      subtitle: synthCardSub(1, voice2.name, voice2.path, player2.name),
      actions: synthQuick(2, songDeck2, arp2.on, caps.arp2 ? () => { const on = !arp2.on; setArp2(x => ({ ...x, on })); tp.arp2On(on); } : undefined),
      foot: <VolSlider label="Vol" value={voice2.vol} onChange={v => setVoice2(x => ({ ...x, vol: v }))} onCommit={v => tp.voice2Vol(v)} disabled={!connected} />,
      body: <SubMenu getItems={() => sections.filter(x => x.parent === 'synthesizerB').sort((a, b) => ord(a.id) - ord(b.id))} onOpen={setRoute} accent={THEME.synthB.accent} tint={THEME.synthB.tint} />,
    },
    // VOICE POOL — runtime engine allocation (4-voice pool builds only; @STATE.pool). Redistribute the
    // 8 Dexed engines among the 4 voices live, trading voice count for per-voice polyphony. No reflash.
    {
      id: 'voicepool', title: 'Voice Pool', show: pool.has, accent: THEME.synthA.accent, tint: THEME.synthA.tint,
      value: poolValue,
      body: poolBody,
    },
    // AUDIO LOOP — gated on caps.audioloop (the COUNT of loops the device could allocate;
    // 0 on a board with no spare RAM). Records the MASTER MIX as audio (not MIDI) into
    // bar-locked, crossfaded loops. See planning/audio-looper/DESIGN.md.
    {
      id: 'audioloop', title: 'Audio Loop', show: caps.audioloop > 0 || !!unavail.audioloop, disabledReason: unavail.audioloop,
      accent: THEME.audioloop.accent, tint: THEME.audioloop.tint,
      value: (caps.audioloop > 1 ? 'Loop ' + (aloop.sel + 1) + ' · ' : '') + AL_STATES[alSt()]
             + '  ·  ' + aloop.bars + ' bar' + (aloop.bars > 1 ? 's' : '') + (aloop.mono ? ' · mono' : ''),
      progress: alSt() >= 2 ? alProg() : undefined,
      foot: <VolSlider label="Vol" value={aloop.level} max={100} onChange={v => setAloop(a => ({ ...a, level: v }))} onCommit={v => tp.audioLoopLevel(v)} disabled={!connected} />,
      actions: (<>
        <HdrBtn label="●" onPress={alRecord} />
        <HdrBtn label="■" stop onPress={alStopBtn} />
      </>),
      body: (
        <>
          <Text style={s.muted}>Records the actual audio coming out of the device — everything in the mix. Hit Record and it starts capturing on the next bar downbeat, then loops seamlessly. Overdub layers more on top.</Text>
          {caps.audioloop > 1 && (
            <Row><Text style={[s.muted, { flex: 1 }]}>Loop</Text>
              {Array.from({ length: caps.audioloop }, (_, i) => (
                <Pressable key={i} style={[s.pill, aloop.sel === i && s.pillOn]} onPress={() => setAlSel(i)}>
                  <Text style={s.text}>{i + 1}{aloop.st[i] >= 2 ? ' ♪' : ''}</Text>
                </Pressable>))}
            </Row>
          )}
          <Row><Text style={[s.muted, { flex: 1 }]}>Bars</Text>
            {[1, 2, 4, 8].map(n => (
              <Pressable key={n} style={[s.pill, aloop.bars === n && s.pillOn, !alFits(n) && { opacity: 0.35 }]}
                disabled={!alFits(n)} onPress={() => setAlBars(n)}>
                <Text style={s.text}>{n}</Text>
              </Pressable>))}
          </Row>
          <Text style={s.muted}>
            {aloop.bars} bar{aloop.bars > 1 ? 's' : ''} ≈ {alBarsSecs(aloop.bars).toFixed(1)}s
            {aloop.capS > 0 ? '  ·  room for ' + aloop.capS.toFixed(1) + 's' + (aloop.mono ? ' (mono)' : '') : ''}
          </Text>
          <Row><View style={{ flex: 1 }}>
              <Text style={s.text}>Mono</Text>
              <Text style={s.muted}>Half the memory — twice the loop length.</Text>
            </View>
            <Switch value={aloop.mono} onValueChange={setAlMono} /></Row>
          <Row><View style={{ flex: 1 }}>
              <Text style={s.text}>Follow tempo</Text>
              <Text style={s.muted}>Keeps the loop locked to the master BPM; pitch shifts with tempo.</Text>
            </View>
            <Switch value={aloop.follow} onValueChange={setAlFollow} /></Row>
          <Row>
            <Pressable style={[s.btn, s.grow1, alSt() === 2 && s.btnOn]} onPress={alRecord}><Text style={s.btnText}>●  Record</Text></Pressable>
            <Pressable style={[s.btn, s.grow1, alSt() === 3 && s.btnOn]} onPress={alOverdub}><Text style={s.btnText}>⊕  Overdub</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={alStopBtn}><Text style={s.btnText}>■  Stop</Text></Pressable>
          </Row>
          <Row>
            <Text style={[s.text, { flex: 1 }]}>{AL_STATES[alSt()]}</Text>
            <Pressable style={[s.btn, s.btnGhost]} onPress={alSaveBtn}><Text style={s.btnText}>Save .wav</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost]} onPress={alClearBtn}><Text style={s.btnText}>Clear</Text></Pressable>
          </Row>
        </>
      ),
    },
    // USB AUDIO — the box as a 24-bit/48k USB sound card (build-flag gated, caps.usbaudio →
    // TDSP_USB_AUDIO). Host audio streams IN to the mix bus and the full mix streams back OUT
    // to the host for recording. Shows the live stream status + the USB-in return level.
    // See planning/usb-audio-mixkit/DESIGN.md.
    {
      id: 'usbaudio', title: 'USB Audio', show: caps.usbaudio || !!unavail.usbaudio, disabledReason: unavail.usbaudio,
      accent: THEME.usb.accent, tint: THEME.usb.tint,
      value: (usbAudio.active ? 'Streaming' : 'Idle') + '  ·  24-bit / 48 kHz',
      foot: <VolSlider label="USB In" value={usbAudio.gain} onChange={v => setUsbAudio(u => ({ ...u, gain: v }))} onCommit={v => tp.usbAudioGain(v)} disabled={!connected} />,
      body: (
        <>
          <Text style={s.muted}>The device appears to your computer as a class-compliant 24-bit / 48 kHz sound card. Audio played on the computer comes IN and joins the mix; the whole mix streams back OUT so you can record the performance in a DAW.</Text>
          <Row>
            <Text style={[s.text, { flex: 1 }]}>Host stream</Text>
            <View style={[s.pill, usbAudio.active && s.pillOn]}>
              <Text style={s.text}>{usbAudio.active ? '● Streaming' : '○ Idle'}</Text>
            </View>
          </Row>
          <Text style={s.sectionLbl}>USB In level</Text>
          <Text style={s.muted}>How loud the computer's audio sits in the mix. Your computer's own volume still controls the stream itself.</Text>
          <VolSlider label="USB In" value={usbAudio.gain} onChange={v => setUsbAudio(u => ({ ...u, gain: v }))} onCommit={v => tp.usbAudioGain(v)} disabled={!connected} />
          {(usbAudio.rxover > 0 || usbAudio.txunder > 0) && (
            <Text style={s.muted}>Clock drift: {usbAudio.rxover} in-overrun{usbAudio.rxover === 1 ? '' : 's'} · {usbAudio.txunder} out-underrun{usbAudio.txunder === 1 ? '' : 's'}</Text>
          )}
        </>
      ),
    },
    // MIDI PLAYER — select a song; Play/Stop in the header. A Synthesizer A sub-page.
    {
      id: 'player', title: 'MIDI Player', show: false, parent: 'synthesizer',
      value: playerValue(songDeck1), progress: playerProgress(songDeck1),
      actions: playerActions(songDeck1),
      body: playerBody(songDeck1),
    },
    // ARPEGGIATOR — a Synthesizer A sub-page.
    {
      id: 'arp', title: 'Arpeggiator', show: false, parent: 'synthesizer',
      value: arpValue(arpSlot1), actions: arpActions(arpSlot1),
      body: arpBody(arpSlot1),
    },
    // MIDI PLAYER 2 — the voice-2 song player (@SONG2*), so two songs play at once. A Synthesizer
    // B sub-page; caps.voice2 gates the parent card, so this only appears on voice-2 builds.
    {
      id: 'player2', title: 'MIDI Player 2', show: false, parent: caps.voice2 ? 'synthesizerB' : undefined, accent: C.accent2,
      value: playerValue(songDeck2), progress: playerProgress(songDeck2),
      actions: playerActions(songDeck2),
      body: playerBody(songDeck2),
    },
    // ARPEGGIATOR 2 — build-flag gated (caps.arp2). A full clone of the arp, on the Voices-2
    // keyboard voice only (@ARP2*); the main arp is untouched.
    {
      id: 'arp2', title: 'Arpeggiator 2', show: false, parent: caps.arp2 ? 'synthesizerB' : undefined, accent: C.accent2,   // Synthesizer B sub-page
      value: arpValue(arpSlot2), actions: arpActions(arpSlot2),
      body: arpBody(arpSlot2),
    },
    // DRUMS — a TOP-LEVEL Track card built through the SAME makeTrackCard factory as the synth voices
    // (its Drum Loops + Kit children come from drumCard.children, spread above). The header ▶ ■ transport
    // plays/stops the selected groove; shown on drum-capable builds (cat.hasDrums).
    drumCard.parent,
    // TAC5212 — codec output level + DAC high-pass filter. A Settings sub-page (Settings submenu).
    {
      id: 'codec', title: 'TAC5212', show: false, parent: 'settings',
      value: (vol <= 0 ? 'Muted' : Math.round(vol) + ' / 100') + '  ·  HPF ' + HPF_MODES[hpf].label,
      actions: (<>
        <HdrBtn label="−" stop onPress={() => stepVol(-1)} />
        <HdrBtn label="＋" onPress={() => stepVol(1)} />
      </>),
      body: (
        <>
          <Text style={s.muted}>TAC5212 headphone output (OUT1/OUT2). This is the master DAC level — the same control as the header VOL.</Text>
          <Text style={s.sectionLbl}>Output volume</Text>
          <View style={s.volRow}>
            <Pressable style={s.pill} onPress={() => stepVol(-1)}><Text style={s.text}>−</Text></Pressable>
            <ThrottledSlider max={100} value={vol} onChange={setVol} onCommit={v => tp.masterVolume(v)} disabled={!connected} />
            <Pressable style={s.pill} onPress={() => stepVol(1)}><Text style={s.text}>＋</Text></Pressable>
          </View>
          <Text style={s.muted}>{vol <= 0 ? 'Muted' : Math.round(vol) + ' / 100  ·  ' + volDb(vol) + ' dB'}</Text>

          <Text style={s.sectionLbl}>DAC high-pass filter</Text>
          <Row><Text style={[s.muted, { flex: 1 }]}>Enabled</Text>
            <Switch value={hpf !== 0} onValueChange={toggleHpf} /></Row>
          <Row><Text style={[s.muted, { flex: 1 }]}>Cutoff</Text>
            {HPF_MODES.map(m => <Pressable key={m.mode} style={[s.pill, hpf === m.mode && s.pillOn]} onPress={() => setHpfMode(m.mode)}><Text style={s.text}>{m.label}</Text></Pressable>)}</Row>
          <Text style={s.muted}>A sub-audio high-pass on the DAC output that blocks DC offset and rumble. Off = all-pass; higher cutoffs trim more low end.</Text>
        </>
      ),
    },
    // SETTINGS — a submenu grouping the system pages (Connection, TAC5212). Its page lists those
    // as cards; tapping one opens that child's own existing page (Back returns here, per parent).
    {
      id: 'settings', title: 'Settings', show: true, status: 'system', accent: THEME.settings.accent, tint: THEME.settings.tint,
      body: <SubMenu getItems={() => sections.filter(x => x.parent === 'settings')} onOpen={setRoute} accent={THEME.settings.accent} tint={THEME.settings.tint} />,
    },
  ];

  // Display order (homepage cards + page routing). Performance sections first
  // (play → pick a voice → tempo → arp → drums), then system (connection, BT, codec).
  // Unlisted ids fall to the end in their definition order (stable sort).
  // Order for the home grid AND for each submenu's children (SubMenu sorts by this too).
  const SECTION_ORDER = ['synthesizer', 'synthesizerB', 'synthX2', 'synthX3', 'drumtrack', 'reverb', 'audioloop', 'usbaudio', 'tempo', 'bt', 'settings',
    'player', 'synth', 'arp', 'player2', 'synth2', 'arp2', 'bpm', 'metro', 'conn', 'codec'];   // all synths: MIDI Player, Synth/Voices, Arp
  // NB: the drum children (drumtrackp/drumtrackv) and generated-synth children (synthXNp/v/a) are NOT
  // listed here — like all makeTrackCard children they fall to ord()=999 and keep their push order.
  const ord = (id: string) => { const i = SECTION_ORDER.indexOf(id); return i < 0 ? 999 : i; };
  const visible = sections.filter(x => x.show).sort((a, b) => ord(a.id) - ord(b.id));
  // A routed page may be a home card OR a submenu sub-page (parent set, not in `visible`), so
  // resolve it against ALL sections.
  const cur = route === 'home' ? null : sections.find(x => x.id === route);
  // Full-height pages stay mounted so their scroll/folder position survives navigation. Include
  // routable sub-pages (parent set) too, not just home cards, so e.g. the Synth/Voices browser
  // reached via a submenu still persists.
  const fullPages = sections.filter(x => (x.show || x.parent) && x.fullHeight);

  return (
    <View style={s.app}>
      {/* ===== header: brand, connect, master volume (global, on every page) ===== */}
      <View style={s.header}>
        <View style={s.brandRow}>
          <View style={[s.dot, connected && s.dotOn]} />
          <Text style={s.brand}>T-DSP</Text>
          <View style={{ flex: 1 }} />
          <Pressable style={s.btn} onPress={() => (connected || connecting ? userDisconnect() : userConnect())}>
            <Text style={s.btnText}>{connected ? 'Disconnect App' : connecting ? 'Cancel' : 'Connect App'}</Text>
          </Pressable>
        </View>
        <Text style={s.statline}>{headerStatus}</Text>
        <BeatStrip sig={metro.sig} bpm={bpm} active={connected && (metro.on || player.playing || !!drums.playing)} live={beatFeed} />
        {/* ===== MASTER TRANSPORT (always in the header): the metronome is the app-wide clock.
            Play defines the downbeat and runs the grid everything (players, drums, arps) locks to;
            Stop clears the stage. Mute toggles only whether you HEAR the click — the clock runs
            either way. Buttons disable when not connected (like the VOL row). ===== */}
        <View style={s.transportRow}>
          <Pressable style={[s.tBtn, metro.on && s.tBtnOn]} onPress={playMetro} disabled={!connected}
            accessibilityLabel={metro.on ? 'Transport running — restart the downbeat' : 'Start the transport'}>
            <Text style={[s.tBtnText, metro.on && s.tBtnOnText]}>▶</Text></Pressable>
          <Pressable style={[s.tBtn, s.tBtnGhost]} onPress={stopMetro} disabled={!connected}
            accessibilityLabel="Stop everything">
            <Text style={s.tBtnText}>■</Text></Pressable>
          <Pressable style={[s.tBtn, s.tBtnGhost, !metro.muted && s.tBtnOn]} disabled={!connected}
            onPress={() => { const muted = !metro.muted; setMetro(m => ({ ...m, muted })); tp.metronomeMute(muted); }}
            accessibilityLabel={metro.muted ? 'Click muted — tap to hear it' : 'Click audible — tap to mute'}>
            <Text style={[s.tBtnText, !metro.muted && s.tBtnOnText]}>{metro.muted ? '🔇' : '🔊'}</Text></Pressable>
          <View style={{ flex: 1 }} />
          <Pressable style={s.tBtn} onPress={() => stepBpm(-1)} disabled={!connected}><Text style={s.tBtnText}>−</Text></Pressable>
          <Text style={s.tBpm}>{Math.round(bpm)}<Text style={s.tBpmUnit}> BPM</Text></Text>
          <Pressable style={s.tBtn} onPress={() => stepBpm(1)} disabled={!connected}><Text style={s.tBtnText}>＋</Text></Pressable>
          {/* Tempo lock: off ⇒ the sole piece of content you start sets the BPM; on ⇒ the tempo is held. */}
          <Pressable style={[s.tBtn, s.tBtnGhost, metro.locked && s.tBtnOn]} disabled={!connected}
            onPress={() => { const locked = !metro.locked; setMetro(m => ({ ...m, locked })); tp.metronomeLock(locked); }}
            accessibilityLabel={metro.locked ? 'Tempo locked — tap to let content set the BPM' : 'Tempo follows content — tap to lock'}>
            <Text style={[s.tBtnText, metro.locked && s.tBtnOnText]}>{metro.locked ? '🔒' : '🔓'}</Text></Pressable>
        </View>
        <View style={s.volRow}>
          <Text style={s.volLbl}>VOL</Text>
          <ThrottledSlider max={100} value={vol} onChange={setVol} onCommit={v => tp.masterVolume(v)} disabled={!connected} />
          <Text style={s.volVal}>{Math.round(vol)}</Text>
        </View>
      </View>

      {!connected && (
        <View style={s.connectHome}>
          {/* Transport picker. Only offered while disconnected, and frozen mid-connect:
              switching rebuilds `tp`, which must never happen under a live/opening link. */}
          <View style={s.segRow}>
            <Pressable style={[s.seg, tkind === 'default' && s.segOn]} disabled={connecting}
                       onPress={() => setTkind('default')}>
              <Text style={[s.segText, tkind === 'default' && s.segTextOn]}>{DEFAULT_TP_LABEL}</Text>
            </Pressable>
            <Pressable style={[s.seg, tkind === 'wifi' && s.segOn]} disabled={connecting}
                       onPress={() => setTkind('wifi')}>
              <Text style={[s.segText, tkind === 'wifi' && s.segTextOn]}>Wi-Fi</Text>
            </Pressable>
          </View>
          {tkind === 'wifi' && (
            <>
              {/* Discovered devices (mDNS _tdsp._tcp). Tapping one fills in its resolved
                  address, so you never hunt for an IP — and several T-DSPs on one LAN each
                  get a row. Hidden on web, which can't browse mDNS. */}
              {discoRef.current.supported && (
                <View style={s.devWrap}>
                  <View style={s.devHead}>
                    <Text style={s.muted}>{found.length ? `Found ${found.length} device${found.length > 1 ? 's' : ''}` : scanning ? 'Scanning for T-DSP devices…' : 'No devices found'}</Text>
                    {scanning && <ActivityIndicator color={C.accent} size="small" />}
                  </View>
                  {found.map(d => {
                    const sel = wifiHost.trim() === d.host;
                    return (
                      <Pressable key={d.id} style={[s.devRow, sel && s.devRowOn]} disabled={connecting}
                                 onPress={() => setWifiHost(d.host)}>
                        <Text style={s.devName}>{d.name}</Text>
                        <Text style={s.devAddr}>{d.host}:{d.port}{d.a2dp === false ? '  ·  no BT audio' : ''}</Text>
                      </Pressable>
                    );
                  })}
                </View>
              )}
              <TextInput style={[s.input, s.hostInput]} value={wifiHost} onChangeText={setWifiHost}
                         editable={!connecting} placeholder="tdsp.local" placeholderTextColor={C.muted}
                         autoCapitalize="none" autoCorrect={false} keyboardType="url" />
              <Text style={s.hostHint}>
                {discoRef.current.supported
                  ? 'Tap a device above, or type a host — blank uses tdsp.local. An IP or host:port works too.'
                  : 'Blank uses tdsp.local. An IP or host:port works too.'}
              </Text>
            </>
          )}
          {/* While connecting, this big button cancels the attempt (and suppresses auto-reconnect)
              so you can stop it from the connecting state, not just once connected. */}
          <Pressable style={[s.btn, s.connectBig, connecting && s.btnGhost]} onPress={() => (connecting ? userDisconnect() : userConnect())}>
            <Text style={s.connectBigText}>{connecting ? 'Cancel' : 'Connect App'}</Text>
          </Pressable>
          <Text style={[s.muted, { textAlign: 'center', marginTop: 14 }]}>
            {connecting
              ? (tkind === 'wifi' ? `Connecting to ${wifiHost.trim() || 'tdsp.local'}…`
                 : Platform.OS === 'web' ? 'Opening the serial port…' : 'Searching for your T-DSP over Bluetooth…')
                : `Connect the app to your T-DSP over ${TP_LABEL[tp.name]} to begin.`}
          </Text>
        </View>
      )}

      {/* Connected but the catalog is still streaming: show a load screen instead of the
          half-populated (broken-looking) homepage. Determinate bar when the device announced
          sizes; otherwise an indeterminate spinner. */}
      {connected && !loaded && <LoadScreen bus={progBus} tpLabel={TP_LABEL[tp.name]} />}

      {connected && loaded && (
        <View style={{ flex: 1 }}>
          {/* ===== HOMEPAGE: a responsive grid of section cards ===== */}
          {!cur && (
            <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 400 }}>
              <View style={s.home}>
                {visible.map(sec => (
                  <View key={sec.id} style={[s.cell, { width: `${100 / cols}%` }]}>
                    <Card title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} progress={sec.progress} actions={sec.actions} foot={sec.foot}
                      onPress={() => setRoute(sec.id)} style={s.cardGrid} accent={sec.accent} topRight={sec.topRight} disabledReason={sec.disabledReason} />
                  </View>
                ))}
              </View>
            </ScrollView>
          )}

          {/* ===== a normal (scrolling) section page ===== */}
          {cur && !cur.fullHeight && (
            <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 400 }}>
              <View style={s.page}>
                {cur.asCard
                  ? <Card title={cur.title} value={cur.value} status={cur.status} subtitle={cur.subtitle} progress={cur.progress} actions={cur.actions} foot={cur.foot} accent={cur.accent} topRight={cur.topRight} onBack={() => setRoute(cur.parent || 'home')} />
                  : <PageHeader title={cur.title} value={cur.value} status={cur.status} subtitle={cur.subtitle} progress={cur.progress} actions={cur.actions} onBack={() => setRoute(cur.parent || 'home')} accent={cur.accent} topRight={cur.topRight} />}
                <View style={s.pageBody}>{cur.body}</View>
              </View>
            </ScrollView>
          )}

          {/* ===== full-height pages: stay MOUNTED (just hidden when inactive) so the
                  selected folder AND the picker's scroll position persist across nav ===== */}
          {fullPages.map(sec => (
            <View key={sec.id} style={[s.page, { flex: 1 }, route !== sec.id && s.hidden]}>
              <PageHeader title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} progress={sec.progress} actions={sec.actions} onBack={() => setRoute(sec.parent || 'home')} accent={sec.accent} topRight={sec.topRight} />
              <View style={[s.pageBody, { flex: 1 }]}>{sec.body}</View>
            </View>
          ))}
        </View>
      )}
    </View>
  );
}

const s = StyleSheet.create({
  app: { flex: 1, backgroundColor: C.bg, paddingTop: Platform.OS === 'web' ? 12 : 52 },
  header: { paddingHorizontal: 14, paddingVertical: 8, borderBottomWidth: 1, borderBottomColor: C.border, backgroundColor: C.bg },
  brandRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  dot: { width: 11, height: 11, borderRadius: 6, backgroundColor: '#da3633' },
  dotOn: { backgroundColor: C.accent },
  brand: { color: C.text, fontWeight: '800', fontSize: 18, letterSpacing: 0.5 },
  statline: { color: C.muted, fontSize: 12, marginTop: 3 },
  // header beat lights (BeatStrip) — one dot per beat of the bar
  beatStrip: { flexDirection: 'row', alignItems: 'center', gap: 7, marginTop: 6, height: 14 },
  beatDot: { width: 12, height: 12, borderRadius: 6, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border },
  beatDotDown: { borderColor: 'rgba(227,179,65,0.55)' },   // downbeat marked even when unlit
  beatDotOn: { backgroundColor: C.accent, borderColor: C.accent, shadowColor: C.accent, shadowOpacity: 0.9, shadowRadius: 6, elevation: 4, transform: [{ scale: 1.18 }] },
  beatDotDownOn: { backgroundColor: DOWNBEAT, borderColor: DOWNBEAT, shadowColor: DOWNBEAT, shadowOpacity: 0.9, shadowRadius: 6, elevation: 4, transform: [{ scale: 1.18 }] },
  volRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 2 },
  volLbl: { color: C.muted, fontSize: 11, width: 26 },
  volVal: { color: C.text, fontSize: 13, width: 28, textAlign: 'right' },
  // Master transport bar (metronome = the clock): Play / Stop / Mute on the left, BPM on the right.
  transportRow: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 6 },
  tBtn: { minWidth: 40, height: 34, paddingHorizontal: 10, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  tBtnGhost: { backgroundColor: 'transparent' },
  tBtnOn: { backgroundColor: '#238636', borderColor: '#238636' },   // transport running = lit green
  tBtnText: { color: C.text, fontSize: 16, fontWeight: '700' },
  tBtnOnText: { color: '#fff' },
  tBpm: { color: C.text, fontSize: 18, fontWeight: '800', minWidth: 74, textAlign: 'center' },
  tBpmUnit: { color: C.muted, fontSize: 11, fontWeight: '600' },
  // homepage grid of cards; capped width so it reads well on a wide desktop window too
  home: { flexDirection: 'row', flexWrap: 'wrap', padding: 5, maxWidth: 1040, width: '100%', alignSelf: 'center' },
  cell: { padding: 5 },                                  // grid gutter (width % set inline per column count)
  cardGrid: { marginHorizontal: 0, marginTop: 0, height: 176 },      // fixed → every card is the same size (tall enough for instrument + loaded song line)
  submenu: { gap: 10 },                                  // submenu pages stack their cards full-width, one per row
  card: { backgroundColor: C.card, borderWidth: 1, borderColor: C.border, borderRadius: 10, marginHorizontal: 10, marginTop: 8, overflow: 'hidden' },
  cardOff: { opacity: 0.45 },                                   // built-but-unavailable feature (see @STATE unavail)
  cardReason: { color: '#f7b955', fontSize: 13, marginTop: 2 }, // amber "⚠ PSRAM required" line on a greyed card
  cardHead: { flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 14, paddingTop: 12 },
  cardActions: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 14, paddingBottom: 12, marginTop: 'auto' },   // pinned to the card bottom
  cardFoot: { paddingHorizontal: 14, paddingTop: 6, paddingBottom: 8, gap: 6 },   // inline controls on the tile (volume slider / reverb on+mix)
  hidden: { display: 'none' },
  drawerLeft: { flex: 1, gap: 2 },
  drawerTitle: { color: C.text, fontWeight: '600', fontSize: 15 },
  drawerValue: { color: C.accent, fontSize: 14, fontWeight: '600' },
  pathLine: { color: C.muted, fontSize: 12, marginTop: 1 },
  tag: { color: C.muted, fontSize: 12, backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 2, borderRadius: 10, overflow: 'hidden', alignSelf: 'flex-start' },
  progTrack: { height: 4, borderRadius: 2, backgroundColor: C.chip, marginTop: 6, overflow: 'hidden' },
  progFill: { height: '100%', borderRadius: 2, backgroundColor: C.accent },
  // A tappable "open" button on each card: bordered chip with generous L/R padding so it's an
  // easy target. The ❯ glyph reads as a modern chevron.
  chevBtn: { marginLeft: 'auto', paddingHorizontal: 16, paddingVertical: 8, borderRadius: 9, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  chev: { color: C.text, fontSize: 16, lineHeight: 18, fontWeight: '700' },
  // Keyboard-ownership toggle in a bordered chip button (matches the ❯/❮ nav buttons).
  kbdBtn: { height: HDR_H, paddingHorizontal: 12, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  // section page
  page: { maxWidth: 720, width: '100%', alignSelf: 'center' },
  pageHead: { paddingHorizontal: 14, paddingTop: 12, paddingBottom: 10, borderBottomWidth: 1, borderBottomColor: C.border },
  pageHeadRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  backBtn: { height: HDR_H, paddingHorizontal: 18, borderRadius: 8, borderWidth: 1, borderColor: C.border, backgroundColor: C.chip, alignItems: 'center', justifyContent: 'center' },
  backTxt: { color: C.text, fontSize: 18, fontWeight: '700', lineHeight: 20 },
  pageTitle: { color: C.text, fontWeight: '800', fontSize: 20 },
  pageBody: { paddingHorizontal: 14, paddingTop: 14, gap: 8 },
  // Synth/Voices: fill-height picker with a breadcrumb nav bar above it
  synthWrap: { flex: 1, gap: 10 },
  navBar: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  upBtn: { width: 38, height: 38, borderRadius: 8, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  upBtnOff: { opacity: 0.35 },
  upTxt: { color: C.text, fontSize: 22, fontWeight: '700', lineHeight: 24 },
  crumbs: { flex: 1 },
  crumbsInner: { alignItems: 'center', paddingRight: 8 },
  crumbItem: { flexDirection: 'row', alignItems: 'center' },
  crumbSep: { color: C.muted, fontSize: 15, marginHorizontal: 4 },
  crumbTxt: { color: C.accent, fontSize: 14, fontWeight: '600' },
  crumbLast: { color: C.text, fontSize: 14, fontWeight: '700' },
  picker: { flex: 1, borderWidth: 1, borderColor: C.border, borderRadius: 7 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 6, flexWrap: 'wrap' },
  muted: { color: C.muted, fontSize: 13 },
  sectionLbl: { color: C.text, fontSize: 13, fontWeight: '700', marginTop: 10, marginBottom: 2 },
  text: { color: C.text, fontSize: 14 },
  btn: { backgroundColor: '#238636', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 7, alignItems: 'center' },
  btnWide: { marginTop: 4 },
  // prominent "Connect App" call-to-action shown on the home screen when disconnected
  connectHome: { marginTop: 56, paddingHorizontal: 24, alignItems: 'center' },
  connectBig: { paddingVertical: 16, paddingHorizontal: 44, minWidth: 240 },
  connectBigText: { color: C.text, fontSize: 17, fontWeight: '700' },
  // Transport picker (connect screen): segmented USB/Bluetooth | Wi-Fi + optional host box.
  segRow: { flexDirection: 'row', borderWidth: 1, borderColor: C.border, borderRadius: 8, overflow: 'hidden', marginBottom: 14 },
  seg: { paddingVertical: 9, paddingHorizontal: 22, backgroundColor: C.card2, minWidth: 104, alignItems: 'center' },
  segOn: { backgroundColor: C.sel },
  segText: { color: C.muted, fontSize: 13, fontWeight: '600' },
  segTextOn: { color: C.text },
  hostInput: { width: 260, marginBottom: 6, textAlign: 'center' },
  hostHint: { color: C.muted, fontSize: 11, textAlign: 'center', maxWidth: 300, marginBottom: 16 },
  // Discovered-device list (mDNS)
  devWrap: { width: 280, marginBottom: 12 },
  devHead: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 8, marginBottom: 8 },
  devRow: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 7, paddingVertical: 8, paddingHorizontal: 12, marginBottom: 6 },
  devRowOn: { borderColor: C.accent, backgroundColor: C.sel },
  devName: { color: C.text, fontSize: 14, fontWeight: '600' },
  devAddr: { color: C.muted, fontSize: 11, marginTop: 2 },
  // catalog loading screen (connected, not yet loaded)
  loadWrap: { flex: 1, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 32, gap: 14 },
  loadTitle: { color: C.text, fontSize: 17, fontWeight: '700' },
  loadTrack: { width: '100%', maxWidth: 360, height: 8, borderRadius: 4, backgroundColor: C.chip, overflow: 'hidden' },
  loadFill: { height: '100%', borderRadius: 4, backgroundColor: C.accent },
  loadSub: { color: C.muted, fontSize: 13, textAlign: 'center' },
  loadHint: { color: C.muted, fontSize: 11, textAlign: 'center', opacity: 0.8 },
  grow1: { flex: 1 },
  headActions: { flexDirection: 'row', alignItems: 'center', gap: 6, flexShrink: 0 },   // content-sized → buttons keep natural width on the page header
  hdrActionsRow: { flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', gap: 8, paddingHorizontal: 14, paddingBottom: 12, marginTop: -2 },
  hdrBtn: { backgroundColor: '#238636', height: HDR_H, paddingHorizontal: 7, borderRadius: 8, flexGrow: 1, flexShrink: 1, flexBasis: 0, minWidth: 34, alignItems: 'center', justifyContent: 'center' },
  hdrBtnStop: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  hdrBtnIdle: { backgroundColor: C.chip },   // play ▶ when NOT playing: dark (green only while playing)
  hdrBtnText: { color: C.text, fontSize: 13, fontWeight: '700' },
  btnGhost: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  btnOn: { backgroundColor: '#238636', borderWidth: 2, borderColor: C.accent },   // PLAYING: green fill + bright border
  btnIdle: { backgroundColor: C.chip },               // NOT playing: dark fill (overrides s.btn green) so the state reads at a glance
  btnRecOn: { borderWidth: 2, borderColor: THEME.recorder.accent },   // armed / capturing (red)
  btnText: { color: C.text, fontSize: 13, fontWeight: '600' },
  input: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 7, color: C.text, paddingHorizontal: 10, paddingVertical: 8, fontSize: 14 },
  list: { maxHeight: 300, borderWidth: 1, borderColor: C.border, borderRadius: 7 },
  browseBox: { height: 340 },   // fixed height so <FolderBrowser>'s flex picker lays out inside a card body
  listBtn: { paddingHorizontal: 12, paddingVertical: 10, borderBottomWidth: 1, borderBottomColor: C.border },
  listBtnSel: { backgroundColor: C.sel },
  // Pattern picker: a wrapping GRID so every one of the 26 patterns is reachable at once
  // (a horizontal strip hid the ones past the first row). Cells stretch to fill each row.
  // Preset/Manual segmented tabs — one arp editor active at a time.
  arpTabs: { flexDirection: 'row', backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 9, padding: 3, gap: 3, marginTop: 2 },
  arpTab: { flex: 1, alignItems: 'center', paddingVertical: 9, borderRadius: 7 },
  arpTabOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  arpTabTxt: { color: C.muted, fontSize: 14, fontWeight: '700' },
  arpTabTxtOn: { color: C.text },
  patGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 2 },
  patCell: { backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 9, borderRadius: 8, minWidth: 74, flexGrow: 1, flexBasis: 74, alignItems: 'center' },
  patCellOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  patCellTxt: { color: C.text, fontSize: 13, fontWeight: '600' },
  pill: { backgroundColor: C.chip, paddingHorizontal: 12, paddingVertical: 6, borderRadius: 14 },
  pillOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  statGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  stat: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 8, paddingVertical: 10, paddingHorizontal: 12, minWidth: 96, flexGrow: 1, alignItems: 'center' },
  statN: { color: C.text, fontSize: 22, fontWeight: '800' },
  statL: { color: C.muted, fontSize: 12, marginTop: 2 },
  statSub: { color: C.accent, fontSize: 11, marginTop: 1 },
});
