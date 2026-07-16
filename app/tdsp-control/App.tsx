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

const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };
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
type AppState = { end: EndMode };
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

// Homepage card: the section's title, live value, and header controls. Fixed size so
// every card matches. Title/value sit on top; controls always sit on their own row at
// the bottom (a card is far narrower than the window, so they never share the title's
// line). Controls are nested Pressables → they fire without navigating the card.
function Card({ title, value, status, subtitle, actions, progress, onPress, style }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; progress?: number; onPress: () => void; style?: any }) {
  return (
    <Pressable style={[s.card, style]} onPress={onPress}>
      <View style={s.cardHead}>
        <View style={s.drawerLeft}>
          <Text style={s.drawerTitle} numberOfLines={1}>{title}</Text>
          {subtitle ?? <Subtitle value={value} status={status} />}
          {progress != null && <ProgressBar value={progress} />}
        </View>
        <Text style={s.chev}>›</Text>
      </View>
      {!!actions && <View style={s.cardActions}>{actions}</View>}
    </Pressable>
  );
}

// Section page header: a back arrow + the section title/value, with the same controls
// available (on the right when wide, on their own row when narrow).
function PageHeader({ title, value, status, subtitle, actions, progress, onBack }:
  { title: string; value?: string; status?: string; subtitle?: React.ReactNode; actions?: React.ReactNode; progress?: number; onBack: () => void }) {
  const { width } = useWindowDimensions();
  const narrow = width < 640;
  return (
    <View style={s.pageHead}>
      <View style={s.pageHeadRow}>
        <Pressable style={s.backBtn} onPress={onBack}><Text style={s.backTxt}>‹</Text></Pressable>
        <View style={s.drawerLeft}>
          <Text style={s.pageTitle}>{title}</Text>
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
  // `song` = the selected song's NAME (its stable identity now that there's no index). name = currently-playing title.
  const [player, setPlayer] = useState<{ song: string; playing: boolean; name: string; prog: number }>({ song: '', playing: false, name: '', prog: 0 });
  const [endMode, setEndMode] = useState<EndMode>('stop');   // what the player does when a song ends
  const [bpm, setBpm] = useState(120);
  const [quant, setQuant] = useState(false);           // launch quantize: start songs/grooves on the next bar
  const [songBpm, setSongBpm] = useState(120);            // tempo of the last song that played
  const [selVoice, setSelVoice] = useState('');
  const [selVoiceName, setSelVoiceName] = useState('');   // last-picked instrument name (shown on the card, persists across browsing)
  const [selVoicePath, setSelVoicePath] = useState('');   // full location of the picked instrument (e.g. /dexed/<cart>.syx, or "Bundled") — shown under the name
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
    if (j.arp) setArp({ on: !!j.arp.on, pat: clampIdx(j.arp.pat, ARP_PAT.length), rate: rateIndexFromFw(j.arp.rate | 0), oct: Math.max(1, Math.min(4, j.arp.oct | 0)) || 1, latch: !!j.arp.latch });
    if (j.song) setPlayer(p => ({ ...p, playing: !!j.song.playing, song: j.song.name || p.song, name: j.song.name || p.name, prog: j.song.p != null ? j.song.p / 1000 : (j.song.playing ? -1 : 0) }));
    if (j.drums) setDrums(d => ({ ...d, kit: j.drums.kit | 0, playing: j.drums.playing ? d.playing : null }));
    if (j.voice) {
      if (j.voice.cart) { setSelVoice('c' + j.voice.cart + ':' + (j.voice.cv | 0)); setSelVoiceName(j.voice.name || ''); setSelVoicePath('/dexed/' + j.voice.cart); }
      else if (j.voice.i != null && j.voice.i < 320) { setSelVoice('b' + (j.voice.i | 0)); setSelVoiceName(j.voice.name || ''); setSelVoicePath('Bundled'); }
    }
  }

  // Restore the opaque app-owned state (@APP=). This is the authoritative source for settings
  // the firmware can't derive; it overrides hydrate()'s loop-based end-mode guess and keeps the
  // firmware loop flag in step. Each persisted field is validated + applied here — add new ones
  // alongside `end` and mirror them in appStateRef so persistApp() keeps sending the full blob.
  function hydrateApp(a: any) {
    if (!a || typeof a !== 'object') return;
    if (isEndMode(a.end)) { appStateRef.current.end = a.end; setEndMode(a.end); tp.songLoop(a.end === 'repeat'); }
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
  const browseRef = useRef<ScrollView>(null);              // the folder-browse list
  const pickerY = useRef<Record<string, number>>({});      // saved scroll offset per list, so we can restore on return
  const voiceData: VItem[] = useMemo(() =>
    cart ? cartVoices.map((vn, i) => ({ key: 'c' + cart.rel + ':' + i, label: (i + 1) + '. ' + vn, i }))
      : vpath === '@bundled' ? cat.instruments.map(v => ({ key: 'b' + v.i, label: v.name, i: v.i }))
        : [], [cart, cartVoices, vpath, cat.instruments]);
  const listId = voiceData.length ? (cart ? 'c' + cart.rel : '@bundled') : 'br:' + vpath;   // identity of the currently-shown picker list
  const pickVoice = (it: VItem) => { setSelVoice(it.key); setSelVoiceName(it.label.replace(/^\d+\.\s*/, '')); if (it.key[0] === 'c' && cart) { setSelVoicePath('/dexed/' + cart.rel); tp.dxPick(cart.rel, it.i); } else if (it.key[0] === 'b') { setSelVoicePath('Bundled'); tp.dxVoice(it.i); } };
  const stepVoice = (dir: number) => {
    // In a visible list (a cart's voices or the bundled set), step within it so the
    // selection stays scrolled into view.
    if (voiceData.length) {
      const idx = voiceData.findIndex(d => d.key === selVoice);
      const ni = Math.max(0, Math.min(voiceData.length - 1, (idx < 0 ? 0 : idx) + dir));
      if (voiceData[ni]) pickVoice(voiceData[ni]);
      return;
    }
    // No list open (browsing folders): step within the bundled set. Crossing /dexed cart
    // boundaries isn't possible now that the library is browsed lazily (not held in RAM),
    // so to step through SD voices, open a cart first.
    if (selVoice[0] !== 'c' && cat.instruments.length) {
      const idx = cat.instruments.findIndex(v => 'b' + v.i === selVoice);
      const ni = Math.max(0, Math.min(cat.instruments.length - 1, (idx < 0 ? 0 : idx) + dir));
      const v = cat.instruments[ni]; if (v) { setSelVoice('b' + v.i); setSelVoiceName(v.name); setSelVoicePath('Bundled'); tp.dxVoice(v.i); }
    }
  };
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
    const ni = Math.max(0, Math.min(grooves.length - 1, (idx < 0 ? 0 : idx) + dir));
    const g = grooves[ni]; if (!g) return;
    setDrums(d => { if (d.playing) { tp.playGrooveFile(grooveFile(g)); return { ...d, sel: g.path, playing: g.name }; } return { ...d, sel: g.path }; });
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
    setPlayer(p => { if (p.playing) { tp.songPlay(songArg(sg)); return { ...p, song: sg.name, name: sg.name }; } return { ...p, song: sg.name }; });
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
  const activePresetName = ARP_LIBRARY.find(p => p.id === arpPresetId)?.name || '';
  const playSong = () => { const sg = cat.songs.find(x => x.name === player.song) || cat.songs[0]; if (!sg) return; tp.songPlay(songArg(sg)); setPlayer(p => ({ ...p, song: sg.name, playing: true, name: sg.name, prog: -1 })); };  // -1 until the device reports position
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

  const headerStatus = !connected ? 'Not connected' :
    [cat.engine || 'synth', cat.drumEngine ? cat.drumEngine + ' drums' : '', '♩ ' + Math.round(bpm) + ' BPM', tp.name, bt.conn ? 'BT:' + (bt.peer || 'on') : '', drums.playing ? '♪ ' + drums.playing : ''].filter(Boolean).join('  ·  ');

  // ===== the sections: one entry drives both its homepage card and its page. =====
  // `value`/`status` = the subtitle; `actions` = the header controls; `body` = the page.
  type Section = { id: string; title: string; show: boolean; value?: string; status?: string; subtitle?: React.ReactNode; progress?: number; actions?: React.ReactNode; body: React.ReactNode; fullHeight?: boolean };

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
    // SYNTH / VOICES — folder browser over bundled voices + the whole /dexed library
    {
      id: 'synth', title: 'Synth / Voices', show: true, value: synthValue, subtitle: synthSubtitle, fullHeight: true,
      actions: (<>
        <HdrBtn label="‹ Prev" stop onPress={() => stepVoice(-1)} />
        <HdrBtn label="Next ›" stop onPress={() => stepVoice(1)} />
      </>),
      body: (
        <View style={s.synthWrap}>
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
            <FlatList ref={voiceRef} data={voiceData} style={s.picker} nestedScrollEnabled keyExtractor={d => d.key}
              getItemLayout={(_, index) => ({ length: ROW_H, offset: ROW_H * index, index })}
              onScrollToIndexFailed={() => {}} scrollEventThrottle={32}
              onScroll={e => { pickerY.current[listId] = e.nativeEvent.contentOffset.y; }}
              renderItem={({ item }) => <ListBtn label={item.label} sel={selVoice === item.key} onPress={() => pickVoice(item)} />} />
          ) : libBusy ? (
            <View style={{ padding: 20, alignItems: 'center' }}><ActivityIndicator color={C.accent} /><Text style={[s.muted, { marginTop: 8 }]}>Loading…</Text></View>
          ) : cart ? (
            <Text style={s.muted}>Couldn't read this cart's voices.</Text>
          ) : (
            <ScrollView ref={browseRef} style={s.picker} nestedScrollEnabled scrollEventThrottle={32}
              onScroll={e => { pickerY.current[listId] = e.nativeEvent.contentOffset.y; }}>
              {vpath === '' && <ListBtn label={'★ Bundled voices (' + cat.instruments.length + ')'} onPress={() => setVpath('@bundled')} />}
              {/* /dexed cart library — only on Dexed engines (others have no cart library to browse) */}
              {cat.hasDexed && level.folders.map(f => <ListBtn key={'f' + f} label={'📁 ' + f} onPress={() => setVpath(vpath ? vpath + '/' + f : f)} />)}
              {cat.hasDexed && level.carts.map(c => <ListBtn key={c.rel} label={'🎛 ' + c.name} onPress={() => setCart({ rel: c.rel, name: c.name })} />)}
              {cat.hasDexed && !!libErr && <Text style={[s.muted, { padding: 12 }]}>⚠ SD library: {libErr} — restart the dev server with `expo start --web -c` and hard-reload.</Text>}
              {cat.hasDexed && !libErr && level.folders.length === 0 && level.carts.length === 0 && <Text style={s.muted}>{vpath === '' ? 'No SD library found (/dexed empty?)' : '(empty folder)'}</Text>}
            </ScrollView>
          )}
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
      value: (arp.on ? '' : '(off)  ') + (arpMode === 'preset'
        ? (activePresetName || 'Preset — none picked')
        : ARP_PAT[arp.pat] + '  ·  ' + ARP_RATES[arp.rate].label),
      actions: (<>
        <HdrBtn label="‹" stop onPress={() => stepArpNav(-1)} />
        <HdrBtn label="›" stop onPress={() => stepArpNav(1)} />
        <Switch value={arp.on} onValueChange={v => { setArp(a => ({ ...a, on: v })); tp.arpOn(v); }} style={{ marginLeft: 6 }} />
      </>),
      body: (
        <>
          <Row><Text style={[s.muted, { flex: 1 }]}>Enabled</Text>
            <Switch value={arp.on} onValueChange={v => { setArp(a => ({ ...a, on: v })); tp.arpOn(v); }} /></Row>
          {/* Two tabs — Preset vs Manual — so only ONE editor drives the arp at a time and it's
              always clear which. Both write the same device config, so we never show both at once. */}
          <View style={s.arpTabs}>
            <Pressable style={[s.arpTab, arpMode === 'preset' && s.arpTabOn]} onPress={() => setArpMode('preset')}>
              <Text style={[s.arpTabTxt, arpMode === 'preset' && s.arpTabTxtOn]}>Presets</Text>
            </Pressable>
            <Pressable style={[s.arpTab, arpMode === 'manual' && s.arpTabOn]} onPress={enterManualMode}>
              <Text style={[s.arpTabTxt, arpMode === 'manual' && s.arpTabTxtOn]}>Manual</Text>
            </Pressable>
          </View>

          {arpMode === 'preset' ? (
            /* PRESET tab — browse the 238-preset library and apply one. */
            <>
              <Text style={s.muted}>{activePresetName ? 'Active preset: ' + activePresetName : 'Pick a preset — it sets everything (pattern, rate, feel, scale…). Switch to Manual to tweak.'}</Text>
              <ArpPresetBrowser onApply={applyPreset} activeId={arpPresetId} />
            </>
          ) : (
            /* MANUAL tab — hand-build the arp. Editing anything here diverges from a preset. */
            <>
              <Text style={s.muted}>Pattern</Text>
              <View style={s.patGrid}>
                {ARP_PAT.map((p, i) => <Pressable key={i} style={[s.patCell, arp.pat === i && s.patCellOn]} onPress={() => selectPattern(i)}><Text style={s.patCellTxt} numberOfLines={1}>{p}</Text></Pressable>)}
              </View>
              {/* User Sequence: the step-grid editor + shape presets (the "actual arpeggiator preset" pattern). */}
              {arp.pat === PAT_USER_SEQUENCE && <ArpStepGrid steps={seq} onChange={applySeq} />}
              <Row><Text style={[s.muted, { flex: 1 }]}>Rate</Text>
                {ARP_RATES.map((r, i) => <Pressable key={i} style={[s.pill, arp.rate === i && s.pillOn]} onPress={() => { setArp(a => ({ ...a, rate: i })); tp.arpRate(r.fw); setArpPresetId(''); }}><Text style={s.text}>{r.label}</Text></Pressable>)}</Row>
              <Row><Text style={[s.muted, { flex: 1 }]}>Octaves {arp.oct}</Text>
                {[1, 2, 3, 4].map(n => <Pressable key={n} style={[s.pill, arp.oct === n && s.pillOn]} onPress={() => { setArp(a => ({ ...a, oct: n })); tp.arpOctaves(n); setArpPresetId(''); }}><Text style={s.text}>{n}</Text></Pressable>)}</Row>
              <Row><Text style={[s.muted, { flex: 1 }]}>Latch</Text>
                <Switch value={arp.latch} onValueChange={v => { setArp(a => ({ ...a, latch: v })); tp.arpLatch(v); setArpPresetId(''); }} /></Row>
              <Pressable style={[s.btn, s.btnGhost, s.btnWide]} onPress={resetArpManual}>
                <Text style={s.btnText}>Reset to plain arp</Text>
              </Pressable>
            </>
          )}
        </>
      ),
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
          <Row><Text style={[s.muted, { flex: 1 }]}>Kit: {cat.drumkits[drums.kit]?.name || '—'}</Text>
            <ScrollView horizontal showsHorizontalScrollIndicator={false}>
              {cat.drumkits.map((k, i) => <Pressable key={i} style={[s.pill, drums.kit === i && s.pillOn]} onPress={() => { setDrums(d => ({ ...d, kit: i })); tp.drumKit(i); }}><Text style={s.text}>{k.name}</Text></Pressable>)}
            </ScrollView></Row>
          <TextInput style={s.input} placeholder="Search grooves…" placeholderTextColor={C.muted}
            value={q.groove} onChangeText={t => setQ(x => ({ ...x, groove: t }))} />
          {cat.grooves.length === 0 ? <Text style={s.muted}>No grooves indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {grooves.map(g => <ListBtn key={g.path} label={(drums.playing === g.name ? '♪ ' : '') + g.name} sel={drums.sel === g.path}
                onPress={() => setDrums(d => ({ ...d, sel: g.path }))} />)}
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
  const SECTION_ORDER = ['player', 'synth', 'bpm', 'arp', 'drums', 'conn', 'bt', 'codec'];
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
                      onPress={() => setRoute(sec.id)} style={s.cardGrid} />
                  </View>
                ))}
              </View>
            </ScrollView>
          )}

          {/* ===== a normal (scrolling) section page ===== */}
          {cur && !cur.fullHeight && (
            <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 400 }}>
              <View style={s.page}>
                <PageHeader title={cur.title} value={cur.value} status={cur.status} subtitle={cur.subtitle} progress={cur.progress} actions={cur.actions} onBack={() => setRoute('home')} />
                <View style={s.pageBody}>{cur.body}</View>
              </View>
            </ScrollView>
          )}

          {/* ===== full-height pages: stay MOUNTED (just hidden when inactive) so the
                  selected folder AND the picker's scroll position persist across nav ===== */}
          {fullPages.map(sec => (
            <View key={sec.id} style={[s.page, { flex: 1 }, route !== sec.id && s.hidden]}>
              <PageHeader title={sec.title} value={sec.value} status={sec.status} subtitle={sec.subtitle} progress={sec.progress} actions={sec.actions} onBack={() => setRoute('home')} />
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
  chev: { color: C.muted, fontSize: 20, marginLeft: 'auto' },
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
