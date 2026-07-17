// App.tsx — T-DSP unified control surface (react-native-web on desktop, native in the app).
// One React/TS codebase over a platform-split transport (Web Serial / BLE). The on-device
// catalog DB (/tdsp/*.ndjson, built by @REINDEX) is the source of truth; browsing is local,
// only actions hit the wire. Old single-file UI preserved as App.old.tsx.
//
// Navigation: a lightweight state router. The homepage lists every section as a card
// (title + live value + the same header controls); tapping a card opens that section's
// own page. No nav library — just a `route` string ('home' | section id).
import React, { useState, useRef, useEffect, useMemo } from 'react';
import { View, Text, Pressable, ScrollView, FlatList, TextInput, Switch, StyleSheet, ActivityIndicator, Platform, Alert, useWindowDimensions } from 'react-native';
import Slider from '@react-native-community/slider';
import { createTransport } from './src/transportFactory';
import { Catalog, EMPTY_CATALOG, loadCatalog, LoadProgress, Song, songArg } from './src/catalog';
import type { Transport, DirPage } from './src/transport';
import ArpStepGrid from './src/ui/ArpStepGrid';
import ArpPresetBrowser from './src/ui/ArpPresetBrowser';
import { ARP_PATTERNS as ARP_PAT, ARP_RATES, rateIndexFromFw, PAT_USER_SEQUENCE, DEFAULT_SHAPE, SeqStep } from './src/arpSeq';
import { applyArpPreset, ArpPreset, ARP_LIBRARY } from './src/arpLibrary';

const EMPTY_DIR: DirPage = { path: '', page: 0, npages: 1, folders: [], carts: [] };
const grooveFile = (g: { path: string; name: string }) => g.path.split('/').pop() || (g.name + '.mid');   // @DRUMF wants filename WITH .mid
const kb = (n: number) => (n / 1024).toFixed(1);   // bytes -> "12.3" KB, for the load progress readout

const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', accent2: '#a371f7', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };
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

// The opaque app-owned state we persist on the device (@APP=) so a reload/reconnect restores
// it. Keep it small (device RAM buffer is fixed) and JSON-serializable; grow it as more
// firmware-invisible UI settings need to survive a reconnect.
type AppState = { end: EndMode; groove?: string };   // groove = last-selected drum groove path (the device only reports the kit, not the picked groove)
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
function Card({ title, value, status, subtitle, actions, progress, onPress, style, accent }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; progress?: number; onPress: () => void; style?: any; accent?: string }) {
  return (
    <View style={[s.card, style, accent && { borderLeftColor: accent, borderLeftWidth: 3 }]}>
      <View style={s.cardHead}>
        <View style={s.drawerLeft}>
          <Text style={[s.drawerTitle, accent && { color: accent }]} numberOfLines={1}>{title}</Text>
          {subtitle ?? <Subtitle value={value} status={status} />}
          {progress != null && <ProgressBar value={progress} />}
        </View>
        <Pressable onPress={onPress} hitSlop={10} style={s.chevBtn}><Text style={s.chev}>›</Text></Pressable>
      </View>
      {!!actions && <View style={s.cardActions}>{actions}</View>}
    </View>
  );
}

// Section page header: a back arrow + the section title/value, with the same controls
// available (on the right when wide, on their own row when narrow).
function PageHeader({ title, value, status, subtitle, actions, progress, onBack, accent }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; progress?: number; onBack: () => void; accent?: string }) {
  const { width } = useWindowDimensions();
  const narrow = width < 640;
  return (
    <View style={[s.pageHead, accent && { borderBottomColor: accent }]}>
      <View style={s.pageHeadRow}>
        <Pressable style={s.backBtn} onPress={onBack}><Text style={s.backTxt}>‹</Text></Pressable>
        <View style={s.drawerLeft}>
          <Text style={[s.pageTitle, accent && { color: accent }]}>{title}</Text>
          {subtitle ?? <Subtitle value={value} status={status} />}
          {progress != null && <ProgressBar value={progress} />}
        </View>
        {!narrow && !!actions && <View style={s.headActions}>{actions}</View>}
      </View>
      {narrow && !!actions && <View style={s.hdrActionsRow}>{actions}</View>}
    </View>
  );
}

// A transport button for a section header (nested Pressable → doesn't navigate the card).
// All header buttons share one uniform width (s.hdrBtn.minWidth).
const HdrBtn = ({ label, onPress, stop }: { label: string; onPress: () => void; stop?: boolean }) => (
  <Pressable onPress={onPress} style={[s.hdrBtn, stop && s.hdrBtnStop]}><Text style={s.hdrBtnText}>{label}</Text></Pressable>
);
const Row = ({ children }: any) => <View style={s.row}>{children}</View>;
// A per-section level slider (0..150 %, 100 = the file's own velocity), independent of the
// master @VOL. Live drag only moves the label; the value is sent on release (onSlidingComplete)
// so a drag never bursts the serial link (mirrors ui.html's @change-not-@input behavior).
const VolSlider = ({ label, value, onChange, onCommit, disabled }:
  { label: string; value: number; onChange: (v: number) => void; onCommit: (v: number) => void; disabled?: boolean }) => (
  <View style={s.volRow}>
    <Text style={[s.muted, { width: 52 }]}>{label}</Text>
    <Slider style={{ flex: 1, height: 34 }} minimumValue={0} maximumValue={150} step={1} value={value}
      minimumTrackTintColor={C.accent} maximumTrackTintColor={C.border} thumbTintColor={C.accent}
      disabled={disabled} onValueChange={onChange} onSlidingComplete={onCommit} />
    <Text style={[s.muted, { width: 44, textAlign: 'right' }]}>{Math.round(value)}%</Text>
  </View>
);
const Stat = ({ label, n, sub }: { label: string; n: number; sub?: string }) => (
  <View style={s.stat}><Text style={s.statN}>{n}</Text><Text style={s.statL}>{label}</Text>{!!sub && <Text style={s.statSub}>{sub}</Text>}</View>
);
const ListBtn = ({ label, sel, onPress }: any) => (
  <Pressable onPress={onPress} style={[s.listBtn, sel && s.listBtnSel]}><Text style={s.text} numberOfLines={1}>{label}</Text></Pressable>
);
const ROW_H = 41;   // fixed list-row height so FlatList.scrollToIndex is reliable
type VItem = { key: string; label: string; i: number };

export default function App() {
  const tpRef = useRef<Transport | null>(null); if (!tpRef.current) tpRef.current = createTransport();
  const tp = tpRef.current;
  const { width } = useWindowDimensions();
  const cols = width < 560 ? 1 : width < 900 ? 2 : 3;   // responsive homepage grid columns
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [userDisc, setUserDisc] = useState(false);      // user tapped Disconnect App → suppress auto-reconnect
  const userDiscRef = useRef(false);                    // synchronous mirror of userDisc so an in-flight connect() can see a cancel immediately
  const connectingRef = useRef(false);                  // synchronous guard so the auto-poll can't double-connect
  const [prog, setProg] = useState<LoadProgress | null>(null);   // catalog load progress (drives the loading screen); null when not loading
  const [loadElapsed, setLoadElapsed] = useState(0);             // seconds on the current catalog load — shows it's alive even if a read stalls
  const manualStopRef = useRef(false);                  // set on user Stop so the resulting @SONGP=-1 isn't treated as a natural song end
  const onSongEndRef = useRef<() => void>(() => {});     // latest "song finished naturally" handler (continue/shuffle); kept in a ref so the @SONGP listener never goes stale
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
  const [bpm, setBpm] = useState(120);
  const [beatFeed, setBeatFeed] = useState<{ i: number; n: number } | null>(null);   // live @BEAT from the device (null = fall back to the local clock)
  const beatStaleRef = useRef<any>(null);
  const [quant, setQuant] = useState(false);           // launch quantize: start songs/grooves on the next bar
  const [metro, setMetro] = useState({ on: false, cap: false, sig: 4, vol: 100 });   // metronome click; cap=true once the device reports @METRO support; sig = beats/bar; vol = click level 0..150 %
  const [songBpm, setSongBpm] = useState(120);            // tempo of the last song that played
  const [selVoice, setSelVoice] = useState('');
  const [selVoiceName, setSelVoiceName] = useState('');   // last-picked instrument name (shown on the card, persists across browsing)
  const [selVoicePath, setSelVoicePath] = useState('');   // full location of the picked instrument (e.g. /dexed/<cart>.syx, or "Bundled") — shown under the name
  // Voices 2 (build-flag gated): a second Dexed voice on the keyboard half of the pool.
  // `caps` = which optional features the connected firmware was built with (from @STATE),
  // so the Voices 2 / Arpeggiator 2 cards SHOW only when compiled in. Each has its own
  // selection + volume + arp state; the folder browser is shared (pickVoice takes a target).
  const [caps, setCaps] = useState({ voice2: false, arp2: false });
  const [voice2, setVoice2] = useState({ on: false, vol: 100, name: '', path: '' });
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
  function hydrate(j: any) {
    if (j.vol != null) setVol(j.vol);
    if (j.hpf != null) { const m = clampIdx(j.hpf, HPF_MODES.length); setHpf(m); if (m) lastHpfRef.current = m; }
    if (j.bpm != null) setBpm(j.bpm);
    // Fallback end-mode guess from the loop flag, for firmware without @APP: loop on ⇒ Repeat;
    // loop off ⇒ keep the app's mode unless it was Repeat (then fall back to Stop). When @APP is
    // present its stored value arrives right after and overrides this (see hydrateApp).
    if (j.loop != null) setEndMode(m => j.loop ? 'repeat' : (m === 'repeat' ? 'stop' : m));
    if (j.quant != null) setQuant(!!j.quant);
    if (j.metro != null) setMetro(m => ({ ...m, on: !!j.metro, cap: true }));   // present only on metronome-capable builds
    if (j.metrosig != null) setMetro(m => ({ ...m, sig: Math.max(1, Math.min(16, j.metrosig | 0)) || 4 }));
    if (j.metrovol != null) setMetro(m => ({ ...m, vol: Math.max(0, Math.min(150, j.metrovol | 0)) }));
    if (j.arp) setArp({ on: !!j.arp.on, pat: clampIdx(j.arp.pat, ARP_PAT.length), rate: rateIndexFromFw(j.arp.rate | 0), oct: Math.max(1, Math.min(4, j.arp.oct | 0)) || 1, latch: !!j.arp.latch });
    if (j.song) setPlayer(p => ({ ...p, playing: !!j.song.playing, song: j.song.name || p.song, name: j.song.name || p.name, prog: j.song.p != null ? j.song.p / 1000 : (j.song.playing ? -1 : 0) }));
    if (j.song?.vol != null) setSongVol(Math.max(0, Math.min(150, j.song.vol | 0)));
    if (j.drums) setDrums(d => ({ ...d, kit: j.drums.kit | 0, playing: j.drums.playing ? d.playing : null }));
    if (j.drums?.vol != null) setDrumVol(Math.max(0, Math.min(150, j.drums.vol | 0)));
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
    if (j.caps) setCaps({ voice2: !!j.caps.voice2, arp2: !!j.caps.arp2 });
    if (j.voice2) setVoice2(v => ({
      on: !!j.voice2.on,
      vol: j.voice2.vol != null ? Math.max(0, Math.min(150, j.voice2.vol | 0)) : v.vol,
      name: j.voice2.name || v.name,
      path: j.voice2.cart ? '/dexed/' + j.voice2.cart : (j.voice2.i != null ? 'Bundled' : v.path),
    }));
    if (j.arp2) setArp2({ on: !!j.arp2.on, pat: clampIdx(j.arp2.pat, ARP_PAT.length), rate: rateIndexFromFw(j.arp2.rate | 0), oct: Math.max(1, Math.min(4, j.arp2.oct | 0)) || 1, latch: !!j.arp2.latch });
  }

  // Restore the opaque app-owned state (@APP=). This is the authoritative source for settings
  // the firmware can't derive; it overrides hydrate()'s loop-based end-mode guess and keeps the
  // firmware loop flag in step. Each persisted field is validated + applied here — add new ones
  // alongside `end` and mirror them in appStateRef so persistApp() keeps sending the full blob.
  function hydrateApp(a: any) {
    if (!a || typeof a !== 'object') return;
    if (isEndMode(a.end)) { appStateRef.current.end = a.end; setEndMode(a.end); tp.songLoop(a.end === 'repeat'); }
    // The device's @STATE reports the drum kit but not which groove is picked, so restore the
    // last-selected groove here — that way the Drums card shows it and ‹/› step from it at startup.
    if (typeof a.groove === 'string' && a.groove) { appStateRef.current.groove = a.groove; setDrums(d => ({ ...d, sel: a.groove })); }
  }

  useEffect(() => tp.onLine(line => {
    if (line.startsWith('@STATE=')) {
      try { hydrate(JSON.parse(line.slice(line.indexOf('=') + 1))); } catch {}
    } else if (line.startsWith('@APP=')) {
      // The device's opaque app-owned state blob (emitted with @STATE and echoed on save).
      // Restore the settings the firmware can't derive; ignore anything unrecognized.
      try { hydrateApp(JSON.parse(line.slice(5))); } catch {}
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
    } else if (line.startsWith('[song]')) {
      // Follow the song's detected tempo: set master BPM to it (song + drums lock to that).
      const m = line.match(/([\d.]+)\s*bpm/); if (m) { const b = Math.round(parseFloat(m[1])); if (b >= 20 && b <= 300) { setSongBpm(b); setBpm(b); tp.masterBpm(b); } }
    }
  }), []);

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
    }
    // A user cancel can surface as a connect rejection (port/scan aborted) — don't toast that.
    catch (e: any) { if (!auto && !userDiscRef.current) notify('Connect failed: ' + e + (Platform.OS === 'web' ? '\n\nClose any control.html tab (one page owns the port), then retry.' : '')); }
    finally { connectingRef.current = false; setConnecting(false); }
  }
  async function disconnect() { try { await tp.disconnect(); } catch {} setConnected(false); setConnecting(false); setLoaded(false); setProg(null); setRoute('home'); }
  // Button handlers wrap connect/disconnect so a *manual* disconnect suppresses auto-reconnect
  // (else the poll below would immediately reconnect and the Disconnect button would do nothing).
  // userDiscRef is set synchronously (before the async setState lands) so connect() sees a
  // mid-connect cancel right away. A user disconnect works from the connecting state too.
  const userConnect = () => { userDiscRef.current = false; setUserDisc(false); connect(); };
  const userDisconnect = () => { userDiscRef.current = true; setUserDisc(true); disconnect(); };

  // Connecting is EXPLICIT: no auto-connect and no auto-reconnect. This poll only reflects
  // a DROPPED link into the UI (flip to "Not connected") — it never opens a connection. You
  // tap Connect App to connect, and once disconnected the app stays put until you do.
  useEffect(() => {
    if (Platform.OS === 'web') return;   // web can't auto-open either (needs a user gesture)
    let cancelled = false;
    const id = setInterval(() => {
      if (cancelled) return;
      if (!tp.isConnected()) { setConnected(false); setLoaded(false); }   // no-op if already false
    }, 4000);
    return () => { cancelled = true; clearInterval(id); };
  }, []);
  // Tick an elapsed-seconds counter while the catalog is loading, so the load screen
  // reads as "working" even if a single @READ stalls (a frozen bar looks broken).
  useEffect(() => {
    if (!(connected && !loaded)) { setLoadElapsed(0); return; }
    const t0 = Date.now();
    const id = setInterval(() => setLoadElapsed(Math.floor((Date.now() - t0) / 1000)), 1000);
    return () => clearInterval(id);
  }, [connected, loaded]);
  async function load() {
    try { const c = await loadCatalog(tp, setProg); setCat(c); setLoaded(true); setProg(null); }
    catch (e: any) {
      setProg(null);
      const yes = Platform.OS === 'web'
        ? (globalThis as any).confirm?.('Catalog load failed: ' + (e?.message || e) + '\n\nRebuild it now (@REINDEX)?')
        : true;
      if (yes) await reindex();
    }
  }
  async function reindex() { setBusy(true); try { await tp.reindex(); await load(); } finally { setBusy(false); } }

  const grooves = useMemo(() => { const t = q.groove.toLowerCase(); return cat.grooves.filter(g => !t || g.name.toLowerCase().includes(t)).slice(0, 500); }, [cat.grooves, q.groove]);

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
  const browseRef = useRef<ScrollView>(null);              // the folder-browse list
  const browseRef2 = useRef<ScrollView>(null);
  const pickerY = useRef<Record<string, number>>({});      // saved scroll offset per list, so we can restore on return
  const voiceData: VItem[] = useMemo(() =>
    cart ? cartVoices.map((vn, i) => ({ key: 'c' + cart.rel + ':' + i, label: (i + 1) + '. ' + vn, i }))
      : vpath === '@bundled' ? cat.instruments.map(v => ({ key: 'b' + v.i, label: v.name, i: v.i }))
        : [], [cart, cartVoices, vpath, cat.instruments]);
  const listId = voiceData.length ? (cart ? 'c' + cart.rel : '@bundled') : 'br:' + vpath;   // identity of the currently-shown picker list
  // Pick a voice for slot `target` (1 = main synth, 2 = the keyboard's Voices-2). Both share
  // the same browse state (cart/folder position) but have independent selection + device
  // targets (@DXVOICE/@DXPICK vs @DXVOICE2/@DXPICK2).
  const pickVoice = (it: VItem, target: 1 | 2 = 1) => {
    const nm = it.label.replace(/^\d+\.\s*/, '');
    const isCart = it.key[0] === 'c' && !!cart;
    const path = isCart ? '/dexed/' + cart!.rel : 'Bundled';
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
  const stepVoice = (dir: number, target: 1 | 2 = 1) => {
    const sel = target === 2 ? selVoice2 : selVoice;
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

  const stepGroove = (dir: number) => {   // step the drum preset; if one is playing, switch to the new one
    if (!grooves.length) return;
    const idx = grooves.findIndex(g => g.path === drums.sel);
    // From no selection (idx<0) the first ‹/› lands on the first/last groove instead of skipping
    // one — same cursor rule the song player's pickNext() uses, so ‹/› work right at startup.
    const base = idx < 0 ? (dir > 0 ? -1 : 0) : idx;
    const ni = Math.max(0, Math.min(grooves.length - 1, base + dir));
    const g = grooves[ni]; if (!g) return;
    setDrums(d => { if (d.playing) { tp.playGrooveFile(grooveFile(g)); return { ...d, sel: g.path, playing: g.name }; } return { ...d, sel: g.path }; });
    persistApp({ groove: g.path });   // remember the pick so it survives an app reconnect
  };
  // The shared "continue rules": which song a skip (‹ ›) or a natural end advances to, per the
  // end-mode. Shuffle → a random *other* song; every other mode → the linear neighbor, wrapping
  // both ways. Both the transport buttons and the auto-advance route through this.
  const pickNext = (dir: number): Song | null => {
    const songs = cat.songs;
    if (!songs.length) return null;
    const idx = songs.findIndex(sg => sg.name === player.song);
    if (endMode === 'shuffle' && songs.length > 1) {
      let r = idx; while (r === idx) r = Math.floor(Math.random() * songs.length);   // never repeat the current song
      return songs[r];
    }
    const base = idx < 0 ? (dir > 0 ? -1 : 0) : idx;
    return songs[((base + dir) % songs.length + songs.length) % songs.length];   // wrap both ways
  };
  const stepSong = (dir: number) => {   // ‹ › skip — follows the end-mode continue rules; plays the new song if one is playing
    const sg = pickNext(dir); if (!sg) return;
    setPlayer(p => { if (p.playing) { tp.songRestart(songArg(sg)); return { ...p, song: sg.name, name: sg.name }; } return { ...p, song: sg.name }; });
  };
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
    const st = applyArpPreset(tp, p);
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
  const applyPreset2 = (p: ArpPreset) => { const st = applyArpPreset(tp, p, 2); setArp2(a => ({ ...a, pat: st.pat, rate: st.rate, oct: st.oct, latch: st.latch })); if (st.seq) setSeq2(st.seq); setArp2PresetId(p.id); };
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
    <HdrBtn label="▶" onPress={A.play} />
    <HdrBtn label="■" stop onPress={A.stop} />
  </>);
  const arpBody = (A: ArpSlotT) => (
    <>
      {/* Play and Stop are independent (not a toggle): Play re-triggers a running arp,
          Stop bypasses it. The active state is shown by which button is highlighted. */}
      <Row>
        <Pressable style={[s.btn, s.grow1, A.arp.on && s.btnOn]} onPress={A.play}>
          <Text style={s.btnText}>▶  {A.arp.on ? 'Restart' : 'Play'}</Text>
        </Pressable>
        <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={A.stop}>
          <Text style={s.btnText}>■  Stop</Text>
        </Pressable>
      </Row>
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
          <Row><Text style={[s.muted, { flex: 1 }]}>Latch</Text>
            <Switch value={A.arp.latch} onValueChange={A.setLatch} /></Row>
          <Pressable style={[s.btn, s.btnGhost, s.btnWide]} onPress={A.resetManual}>
            <Text style={s.btnText}>Reset to plain arp</Text>
          </Pressable>
        </>
      )}
    </>
  );
  const playSong = () => { const sg = cat.songs.find(x => x.name === player.song) || cat.songs[0]; if (!sg) return; tp.songRestart(songArg(sg)); setPlayer(p => ({ ...p, song: sg.name, playing: true, name: sg.name, prog: -1 })); };  // restart from the top on a fresh downbeat; -1 until the device reports position
  const stopSong = () => { manualStopRef.current = true; tp.stopSong(); setPlayer(p => ({ ...p, playing: false, prog: 0 })); };
  const playSongOf = (sg: Song) => { tp.songPlay(songArg(sg)); setPlayer(p => ({ ...p, song: sg.name, playing: true, name: sg.name, prog: -1 })); };
  // Merge a patch into the persisted app-state and push the whole blob to the device (@APP=)
  // so it survives an app reload/reconnect. The device stores it opaquely; the app owns it.
  const persistApp = (patch: Partial<AppState>) => { appStateRef.current = { ...appStateRef.current, ...patch }; tp.saveAppState(appStateRef.current); };
  // End-of-song mode. Only 'repeat' arms the firmware's seamless loop; the rest let the song
  // end (device emits @SONGP=-1) and we advance app-side. The choice is persisted on the device.
  const applyEndMode = (m: EndMode) => { setEndMode(m); tp.songLoop(m === 'repeat'); persistApp({ end: m }); };
  const cycleEndMode = () => { const i = END_MODES.findIndex(m => m.key === endMode); applyEndMode(END_MODES[(i + 1) % END_MODES.length].key); };
  // Runs when a song finishes on its own (not a manual Stop). Kept fresh in a ref so the
  // one-time @SONGP listener always sees the current mode/song/catalog. Continue/Shuffle
  // advance per the shared pickNext rules; 'stop' does nothing; 'repeat' loops in firmware.
  onSongEndRef.current = () => {
    if (endMode === 'continue' || endMode === 'shuffle') { const nx = pickNext(1); if (nx) playSongOf(nx); }
  };
  const playGroove = () => { const g = cat.grooves.find(x => x.path === drums.sel); if (g) { tp.playGrooveFile(grooveFile(g)); setDrums(d => ({ ...d, playing: g.name })); } };
  const stopDrums = () => { tp.stopDrums(); setDrums(d => ({ ...d, playing: null })); };
  // Metronome transport, independent Play/Stop (mirrors the arp). Play sends @METRO=1 even
  // when already running — the firmware restarts the bar from beat 1 — so it re-triggers.
  const playMetro = () => { setMetro(m => ({ ...m, on: true })); tp.metronome(true); };
  const stopMetro = () => { setMetro(m => ({ ...m, on: false })); tp.metronome(false); };

  const headerStatus = !connected ? 'Not connected' :
    [cat.engine || 'synth', cat.drumEngine ? cat.drumEngine + ' drums' : '', '♩ ' + Math.round(bpm) + ' BPM', tp.name, bt.conn ? 'BT:' + (bt.peer || 'on') : '', drums.playing ? '♪ ' + drums.playing : ''].filter(Boolean).join('  ·  ');

  // ===== the sections: one entry drives both its homepage card and its page. =====
  // `value`/`status` = the subtitle; `actions` = the header controls; `body` = the page.
  type Section = { id: string; title: string; show: boolean; value?: string; status?: string; subtitle?: React.ReactNode; progress?: number; actions?: React.ReactNode; body: React.ReactNode; fullHeight?: boolean; accent?: string };

  // The card/page subtitle: the currently-loaded instrument if one is picked, else a
  // summary of where the browser is (folder name or catalog counts).
  const synthValue = selVoiceName
    || (vpath === '@bundled' ? 'Bundled' : vpath ? (vpath.split('/').pop() || '') : (cat.instruments.length ? cat.instruments.length + ' voices + SD library' : 'Library'));
  // When an instrument is loaded, show its name AND full location (folder/cart path). The
  // path truncates from the LEFT (ellipsizeMode "head") so the deepest folder + cart stay
  // visible. No voice picked yet → undefined, so the card falls back to the browse summary.
  const synthSubtitle = selVoiceName ? (
    <>
      <Text style={s.drawerValue} numberOfLines={1}>{selVoiceName}</Text>
      {!!selVoicePath && <Text style={s.pathLine} numberOfLines={1} ellipsizeMode="head">{selVoicePath}</Text>}
    </>
  ) : undefined;

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

  // The folder browser (nav bar + picker), shared by the Synth and Synth/Voices 2 pages.
  // Both share the browse position (cart/folder) but keep their own selection + list ref, and
  // pick into their own voice slot. `target` routes taps to voice 1 or voice 2.
  const voiceBrowserBody = (target: 1 | 2) => {
    const sel = target === 2 ? selVoice2 : selVoice;
    const lref = target === 2 ? voiceRef2 : voiceRef;
    const bref = target === 2 ? browseRef2 : browseRef;
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

  const sections: Section[] = [
    // CONNECTION — catalog stats
    {
      id: 'conn', title: 'Connection', show: true, status: cat.engine || 'connected',
      body: (
        <>
          <Text style={s.muted}>Synth: <Text style={s.text}>{cat.engine || '—'}</Text>   ·   Transport: {tp.name}</Text>
          {!loaded && <Text style={s.muted}>{connected ? 'Loading catalog…' : 'Connect to load the catalog.'}</Text>}
          {loaded && (
            <View style={s.statGrid}>
              <Stat label="Instruments" n={cat.instruments.length} sub="+ SD library" />
              <Stat label="Grooves" n={cat.grooves.length} />
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
      id: 'bt', title: 'Bluetooth', show: cat.hasBt, status: bt.conn ? 'connected' + (bt.peer ? ': ' + bt.peer : '') : 'off',
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
    // TEMPO / BPM — master tempo; song + drums lock to it (@BPM=)
    {
      id: 'bpm', title: 'Tempo', show: true, value: Math.round(bpm) + ' BPM',
      actions: (<>
        <HdrBtn label="−" stop onPress={() => stepBpm(-1)} />
        <HdrBtn label="＋" onPress={() => stepBpm(1)} />
      </>),
      body: (
        <>
          <Text style={{ color: C.text, textAlign: 'center', fontSize: 30, fontWeight: '800' }}>{Math.round(bpm)} <Text style={{ fontSize: 15, color: C.muted }}>BPM</Text></Text>
          <View style={s.volRow}>
            <Pressable style={s.pill} onPress={() => stepBpm(-1)}><Text style={s.text}>−</Text></Pressable>
            <Slider style={{ flex: 1, height: 34 }} minimumValue={40} maximumValue={240} step={1} value={bpm}
              minimumTrackTintColor={C.accent} maximumTrackTintColor={C.border} thumbTintColor={C.accent}
              onValueChange={setBpm} onSlidingComplete={b => tp.masterBpm(b)} />
            <Pressable style={s.pill} onPress={() => stepBpm(1)}><Text style={s.text}>＋</Text></Pressable>
          </View>
          <Pressable style={[s.btn, s.btnGhost]} onPress={() => { const b = player.playing ? songBpm : 120; setBpm(b); tp.masterBpm(b); }}>
            <Text style={s.btnText}>Reset → {player.playing ? songBpm + ' (playing song)' : '120'} BPM</Text></Pressable>
          <Text style={s.muted}>Master tempo — the MIDI song player and drum grooves lock to this. It auto-follows a song's detected tempo on play.</Text>
          <Row><View style={{ flex: 1 }}>
              <Text style={s.text}>Launch quantize</Text>
              <Text style={s.muted}>Start songs & grooves on the next bar so they lock together.</Text>
            </View>
            <Switch value={quant} onValueChange={v => { setQuant(v); tp.launchQuantize(v); }} /></Row>
        </>
      ),
    },
    // METRONOME — on-beat click locked to the master clock. Shown only on builds that
    // report @METRO support (metro.cap, set from @STATE). Follows the master BPM + the
    // playing content's meter automatically — it's a pure clock consumer, no own tempo.
    {
      id: 'metro', title: 'Metronome', show: metro.cap, value: metro.on ? 'Playing' : 'Paused',
      actions: (<>
        <HdrBtn label="▶" onPress={playMetro} />
        <HdrBtn label="■" stop onPress={stopMetro} />
      </>),
      body: (
        <>
          {/* Independent Play/Stop (not a toggle): Play re-triggers the count from beat 1
              even while running; Stop silences it. Active state = which button is lit. */}
          <Row>
            <Pressable style={[s.btn, s.grow1, metro.on && s.btnOn]} onPress={playMetro}>
              <Text style={s.btnText}>▶  {metro.on ? 'Restart' : 'Play'}</Text>
            </Pressable>
            <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={stopMetro}>
              <Text style={s.btnText}>■  Stop</Text>
            </Pressable>
          </Row>
          <Text style={s.muted}>Play starts on the downbeat — the beat lights light beat 1 and count forward from there. Runs at the master tempo ({Math.round(bpm)} BPM); set it on the Tempo card.</Text>
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
      ),
    },
    // SYNTH / VOICES — folder browser over bundled voices + the whole /dexed library
    {
      id: 'synth', title: 'Synth / Voices', show: true, value: synthValue, subtitle: synthSubtitle, fullHeight: true,
      actions: (<>
        <HdrBtn label="‹ Prev" stop onPress={() => stepVoice(-1)} />
        <HdrBtn label="Next ›" stop onPress={() => stepVoice(1)} />
      </>),
      body: (
        <View style={s.synthWrap}>
          {/* Synth output level — the same control as the MIDI Player's Volume (both drive
              @SONGVOL, the synth mix-bus fader), surfaced here since this is the synth page. */}
          <VolSlider label="Volume" value={songVol} onChange={setSongVol} onCommit={v => tp.songVol(v)} disabled={!connected} />
          {voiceBrowserBody(1)}
        </View>
      ),
    },
    // SYNTH / VOICES 2 — build-flag gated (caps.voice2). A full clone of the Synth/Voices
    // page (same folder browser) plus an on/off split toggle: engines 0-3 keep voice 1
    // (song/arp/drums), engines 4-7 are this voice, played live by a USB-host keyboard.
    {
      id: 'synth2', title: 'Synth / Voices 2', show: caps.voice2, fullHeight: true, accent: C.accent2,
      value: (voice2.on ? '' : '(off)  ') + (voice2.name || 'None'),
      subtitle: voice2.name ? (
        <>
          <Text style={s.drawerValue} numberOfLines={1}>{voice2.name}</Text>
          {!!voice2.path && <Text style={s.pathLine} numberOfLines={1} ellipsizeMode="head">{voice2.path}</Text>}
        </>
      ) : undefined,
      actions: (<>
        <HdrBtn label="‹ Prev" stop onPress={() => stepVoice(-1, 2)} />
        <HdrBtn label="Next ›" stop onPress={() => stepVoice(1, 2)} />
      </>),
      body: (
        <View style={s.synthWrap}>
          {/* Enable the split: engines 0-3 keep voice 1 (song/arp/drums), engines 4-7 become
              this voice, played live by a USB-host keyboard. Off = one unified 16-voice pool. */}
          <Row><View style={{ flex: 1 }}>
              <Text style={s.text}>Enable Voices 2 (keyboard split)</Text>
              <Text style={s.muted}>Breaks a USB keyboard off to this voice. Each side drops to 8-voice polyphony.</Text>
            </View>
            <Switch value={voice2.on} onValueChange={v => { setVoice2(x => ({ ...x, on: v })); tp.voice2Enable(v); }} /></Row>
          <VolSlider label="Volume" value={voice2.vol} onChange={v => setVoice2(x => ({ ...x, vol: v }))} onCommit={v => tp.voice2Vol(v)} disabled={!connected || !voice2.on} />
          {voiceBrowserBody(2)}
        </View>
      ),
    },
    // MIDI PLAYER — select a song; Play/Stop in the header (works collapsed); loop toggle
    {
      id: 'player', title: 'MIDI Player', show: true,
      value: (player.playing ? '♪ ' : '') + (player.song || '—'),
      progress: player.playing && player.prog >= 0 ? player.prog : undefined,   // bar shows only once the device reports a position
      actions: (<>
        <HdrBtn label="‹" stop onPress={() => stepSong(-1)} />
        <HdrBtn label="›" stop onPress={() => stepSong(1)} />
        <HdrBtn label="▶" onPress={playSong} />
        <HdrBtn label="■" stop onPress={stopSong} />
        <HdrBtn label={(END_MODES.find(m => m.key === endMode) || END_MODES[3]).icon} stop onPress={cycleEndMode} />
      </>),
      body: (
        <>
          <VolSlider label="Volume" value={songVol} onChange={setSongVol} onCommit={v => tp.songVol(v)} disabled={!connected} />
          {cat.songs.length === 0 ? <Text style={s.muted}>No songs indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {cat.songs.map(sg => <ListBtn key={sg.file || sg.name} label={(player.playing && player.song === sg.name ? '♪ ' : '') + sg.name} sel={player.song === sg.name}
                onPress={() => setPlayer(p => ({ ...p, song: sg.name }))} />)}
            </ScrollView>
          )}
          <Row><Text style={[s.muted, { flex: 1 }]}>When finished</Text>
            {END_MODES.map(m => (
              <Pressable key={m.key} style={[s.pill, endMode === m.key && s.pillOn]} onPress={() => applyEndMode(m.key)}>
                <Text style={s.text}>{m.icon}  {m.label}</Text>
              </Pressable>
            ))}</Row>
        </>
      ),
    },
    // ARPEGGIATOR
    {
      id: 'arp', title: 'Arpeggiator', show: true,
      value: arpValue(arpSlot1), actions: arpActions(arpSlot1),
      body: arpBody(arpSlot1),
    },
    // ARPEGGIATOR 2 — build-flag gated (caps.arp2). A full clone of the arp, on the Voices-2
    // keyboard voice only (@ARP2*); the main arp is untouched.
    {
      id: 'arp2', title: 'Arpeggiator 2', show: caps.arp2, accent: C.accent2,
      value: arpValue(arpSlot2), actions: arpActions(arpSlot2),
      body: arpBody(arpSlot2),
    },
    // DRUMS — only if the built engine renders ch10 drums (hidden e.g. on a no-PSRAM TSF build)
    {
      id: 'drums', title: 'Drums', show: cat.hasDrums,
      value: drums.playing ? '♪ ' + drums.playing : (cat.grooves.find(g => g.path === drums.sel)?.name || cat.drumkits[drums.kit]?.name || '—'),
      actions: (<>
        <HdrBtn label="‹" stop onPress={() => stepGroove(-1)} />
        <HdrBtn label="›" stop onPress={() => stepGroove(1)} />
        <HdrBtn label="▶" onPress={playGroove} />
        <HdrBtn label="■" stop onPress={stopDrums} />
      </>),
      body: (
        <>
          <VolSlider label="Volume" value={drumVol} onChange={setDrumVol} onCommit={v => tp.drumVol(v)} disabled={!connected} />
          <Row><Text style={[s.muted, { flex: 1 }]}>Kit: {cat.drumkits[drums.kit]?.name || '—'}</Text>
            <ScrollView horizontal showsHorizontalScrollIndicator={false}>
              {cat.drumkits.map((k, i) => <Pressable key={i} style={[s.pill, drums.kit === i && s.pillOn]} onPress={() => { setDrums(d => ({ ...d, kit: i })); tp.drumKit(i); }}><Text style={s.text}>{k.name}</Text></Pressable>)}
            </ScrollView></Row>
          <TextInput style={s.input} placeholder="Search grooves…" placeholderTextColor={C.muted}
            value={q.groove} onChangeText={t => setQ(x => ({ ...x, groove: t }))} />
          {cat.grooves.length === 0 ? <Text style={s.muted}>No grooves indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {grooves.map(g => <ListBtn key={g.path} label={(drums.playing === g.name ? '♪ ' : '') + g.name} sel={drums.sel === g.path}
                onPress={() => { setDrums(d => ({ ...d, sel: g.path })); persistApp({ groove: g.path }); }} />)}
            </ScrollView>
          )}
          <Row>
            <Pressable style={[s.btn, s.grow1]} disabled={!drums.sel} onPress={playGroove}><Text style={s.btnText}>▶ Play</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={stopDrums}><Text style={s.btnText}>■ Stop</Text></Pressable>
          </Row>
        </>
      ),
    },
    // TAC5212 — codec output level + DAC high-pass filter
    {
      id: 'codec', title: 'TAC5212', show: true,
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
            <Slider style={{ flex: 1, height: 34 }} minimumValue={0} maximumValue={100} step={1} value={vol}
              minimumTrackTintColor={C.accent} maximumTrackTintColor={C.border} thumbTintColor={C.accent}
              disabled={!connected} onValueChange={setVol} onSlidingComplete={v => tp.masterVolume(v)} />
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
  ];

  // Display order (homepage cards + page routing). Performance sections first
  // (play → pick a voice → tempo → arp → drums), then system (connection, BT, codec).
  // Unlisted ids fall to the end in their definition order (stable sort).
  const SECTION_ORDER = ['player', 'synth', 'synth2', 'bpm', 'metro', 'arp', 'arp2', 'drums', 'conn', 'bt', 'codec'];
  const ord = (id: string) => { const i = SECTION_ORDER.indexOf(id); return i < 0 ? 999 : i; };
  const visible = sections.filter(x => x.show).sort((a, b) => ord(a.id) - ord(b.id));
  const cur = route === 'home' ? null : visible.find(x => x.id === route);
  const fullPages = visible.filter(x => x.fullHeight);   // kept mounted so their scroll/folder position survives navigation

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
        <View style={s.volRow}>
          <Text style={s.volLbl}>VOL</Text>
          <Slider style={{ flex: 1, height: 34 }} minimumValue={0} maximumValue={100} step={1}
            value={vol} minimumTrackTintColor={C.accent} maximumTrackTintColor={C.border} thumbTintColor={C.accent}
            disabled={!connected} onValueChange={setVol} onSlidingComplete={v => tp.masterVolume(v)} />
          <Text style={s.volVal}>{Math.round(vol)}</Text>
        </View>
      </View>

      {!connected && (
        <View style={s.connectHome}>
          {/* While connecting, this big button cancels the attempt (and suppresses auto-reconnect)
              so you can stop it from the connecting state, not just once connected. */}
          <Pressable style={[s.btn, s.connectBig, connecting && s.btnGhost]} onPress={() => (connecting ? userDisconnect() : userConnect())}>
            <Text style={s.connectBigText}>{connecting ? 'Cancel' : 'Connect App'}</Text>
          </Pressable>
          <Text style={[s.muted, { textAlign: 'center', marginTop: 14 }]}>
            {connecting
              ? (Platform.OS === 'web' ? 'Opening the serial port…' : 'Searching for your T-DSP over Bluetooth…')
                : `Connect the app to your T-DSP over ${tp.name} to begin.`}
          </Text>
        </View>
      )}

      {/* Connected but the catalog is still streaming: show a load screen instead of the
          half-populated (broken-looking) homepage. Determinate bar when the device announced
          sizes; otherwise an indeterminate spinner. */}
      {connected && !loaded && (
        <View style={s.loadWrap}>
          <ActivityIndicator color={C.accent} size="large" />
          <Text style={s.loadTitle}>Loading catalog…</Text>
          {prog && prog.index > 0 && prog.det && prog.total > 0 ? (
            // A file is streaming and the device reported sizes: live byte-fraction bar.
            <>
              <View style={s.loadTrack}><View style={[s.loadFill, { width: `${Math.min(100, Math.round(100 * prog.done / prog.total))}%` }]} /></View>
              <Text style={s.loadSub}>{prog.label} · {prog.index}/{prog.count} · {Math.min(100, Math.round(100 * prog.done / prog.total))}% · {kb(prog.done)}/{kb(prog.total)} KB</Text>
            </>
          ) : (
            // Reading the index, or old firmware with no sizes: name the step instead.
            <Text style={s.loadSub}>{prog && prog.index > 0 ? `${prog.label} · ${prog.index}/${prog.count}` : 'Reading catalog index…'}</Text>
          )}
          <Text style={s.loadHint}>{loadElapsed}s elapsed{loadElapsed >= 6 ? ` · streaming over ${tp.name}…` : ''}</Text>
        </View>
      )}

      {connected && loaded && (
        <View style={{ flex: 1 }}>
          {/* ===== HOMEPAGE: a responsive grid of section cards ===== */}
          {!cur && (
            <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 400 }}>
              <View style={s.home}>
                {visible.map(sec => (
                  <View key={sec.id} style={[s.cell, { width: `${100 / cols}%` }]}>
                    <Card title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} progress={sec.progress} actions={sec.actions}
                      onPress={() => setRoute(sec.id)} style={s.cardGrid} accent={sec.accent} />
                  </View>
                ))}
              </View>
            </ScrollView>
          )}

          {/* ===== a normal (scrolling) section page ===== */}
          {cur && !cur.fullHeight && (
            <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 400 }}>
              <View style={s.page}>
                <PageHeader title={cur.title} value={cur.value} status={cur.status} subtitle={cur.subtitle} progress={cur.progress} actions={cur.actions} onBack={() => setRoute('home')} accent={cur.accent} />
                <View style={s.pageBody}>{cur.body}</View>
              </View>
            </ScrollView>
          )}

          {/* ===== full-height pages: stay MOUNTED (just hidden when inactive) so the
                  selected folder AND the picker's scroll position persist across nav ===== */}
          {fullPages.map(sec => (
            <View key={sec.id} style={[s.page, { flex: 1 }, route !== sec.id && s.hidden]}>
              <PageHeader title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} progress={sec.progress} actions={sec.actions} onBack={() => setRoute('home')} accent={sec.accent} />
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
  // homepage grid of cards; capped width so it reads well on a wide desktop window too
  home: { flexDirection: 'row', flexWrap: 'wrap', padding: 5, maxWidth: 1040, width: '100%', alignSelf: 'center' },
  cell: { padding: 5 },                                  // grid gutter (width % set inline per column count)
  cardGrid: { marginHorizontal: 0, marginTop: 0, height: 152 },      // fixed → every card is the same size
  card: { backgroundColor: C.card, borderWidth: 1, borderColor: C.border, borderRadius: 10, marginHorizontal: 10, marginTop: 8, overflow: 'hidden' },
  cardHead: { flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 14, paddingTop: 12 },
  cardActions: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 14, paddingBottom: 12, marginTop: 'auto' },   // pinned to the card bottom
  hidden: { display: 'none' },
  drawerLeft: { flex: 1, gap: 2 },
  drawerTitle: { color: C.text, fontWeight: '600', fontSize: 15 },
  drawerValue: { color: C.accent, fontSize: 14, fontWeight: '600' },
  pathLine: { color: C.muted, fontSize: 12, marginTop: 1 },
  tag: { color: C.muted, fontSize: 12, backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 2, borderRadius: 10, overflow: 'hidden', alignSelf: 'flex-start' },
  progTrack: { height: 4, borderRadius: 2, backgroundColor: C.chip, marginTop: 6, overflow: 'hidden' },
  progFill: { height: '100%', borderRadius: 2, backgroundColor: C.accent },
  chevBtn: { marginLeft: 'auto', paddingHorizontal: 10, paddingVertical: 4, borderRadius: 6 },
  chev: { color: C.muted, fontSize: 22, lineHeight: 22 },
  // section page
  page: { maxWidth: 720, width: '100%', alignSelf: 'center' },
  pageHead: { paddingHorizontal: 14, paddingTop: 12, paddingBottom: 10, borderBottomWidth: 1, borderBottomColor: C.border },
  pageHeadRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  backBtn: { width: 40, height: 40, borderRadius: 8, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  backTxt: { color: C.text, fontSize: 24, fontWeight: '700', lineHeight: 26 },
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
  hdrBtn: { backgroundColor: '#238636', paddingVertical: 9, paddingHorizontal: 10, borderRadius: 6, flexGrow: 1, flexBasis: 0, minWidth: 44, alignItems: 'center' },
  hdrBtnStop: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  hdrBtnText: { color: C.text, fontSize: 15, fontWeight: '700' },
  btnGhost: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  btnOn: { borderWidth: 2, borderColor: C.accent },   // active-transport highlight (arp running)
  btnText: { color: C.text, fontSize: 13, fontWeight: '600' },
  input: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 7, color: C.text, paddingHorizontal: 10, paddingVertical: 8, fontSize: 14 },
  list: { maxHeight: 300, borderWidth: 1, borderColor: C.border, borderRadius: 7 },
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
