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
import { Catalog, EMPTY_CATALOG, loadCatalog } from './src/catalog';
import type { Transport, DirPage } from './src/transport';

const EMPTY_DIR: DirPage = { path: '', page: 0, npages: 1, folders: [], carts: [] };
const grooveFile = (g: { path: string; name: string }) => g.path.split('/').pop() || (g.name + '.mid');   // @DRUMF wants filename WITH .mid

const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };
const ARP_PAT = ['Up', 'Down', 'Up/Down', 'Random'];
const ARP_RATE = ['1/4', '1/8', '1/8T', '1/16', '1/16T', '1/32'];

function notify(msg: string) { if (Platform.OS === 'web') (globalThis as any).alert?.(msg); else Alert.alert('T-DSP', msg); }

// The subtitle line under a section title: the live accent value, else a muted status tag.
const Subtitle = ({ value, status }: { value?: string; status?: string }) =>
  !!value ? <Text style={s.drawerValue} numberOfLines={1}>{value}</Text>
    : !!status ? <Text style={s.tag}>{status}</Text> : null;

// Homepage card: the section's title, live value, and header controls. Fixed size so
// every card matches. Title/value sit on top; controls always sit on their own row at
// the bottom (a card is far narrower than the window, so they never share the title's
// line). Controls are nested Pressables → they fire without navigating the card.
function Card({ title, value, status, actions, onPress, style }:
  { title: string; value?: string; status?: string; actions?: React.ReactNode; onPress: () => void; style?: any }) {
  return (
    <Pressable style={[s.card, style]} onPress={onPress}>
      <View style={s.cardHead}>
        <View style={s.drawerLeft}>
          <Text style={s.drawerTitle} numberOfLines={1}>{title}</Text>
          <Subtitle value={value} status={status} />
        </View>
        <Text style={s.chev}>›</Text>
      </View>
      {!!actions && <View style={s.cardActions}>{actions}</View>}
    </Pressable>
  );
}

// Section page header: a back arrow + the section title/value, with the same controls
// available (on the right when wide, on their own row when narrow).
function PageHeader({ title, value, status, actions, onBack }:
  { title: string; value?: string; status?: string; actions?: React.ReactNode; onBack: () => void }) {
  const { width } = useWindowDimensions();
  const narrow = width < 640;
  return (
    <View style={s.pageHead}>
      <View style={s.pageHeadRow}>
        <Pressable style={s.backBtn} onPress={onBack}><Text style={s.backTxt}>‹</Text></Pressable>
        <View style={s.drawerLeft}>
          <Text style={s.pageTitle}>{title}</Text>
          <Subtitle value={value} status={status} />
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
  const [cat, setCat] = useState<Catalog>(EMPTY_CATALOG);
  const [loaded, setLoaded] = useState(false);
  const [route, setRoute] = useState<string>('home');   // 'home' or a section id
  const [vol, setVol] = useState(80);
  const [bt, setBt] = useState({ conn: false, peer: '' });
  const [arp, setArp] = useState({ on: false, pat: 0, rate: 0, oct: 1, latch: false });
  const [drums, setDrums] = useState<{ kit: number; sel: string | null; playing: string | null }>({ kit: 0, sel: null, playing: null });
  const [player, setPlayer] = useState<{ song: number; playing: boolean; name: string }>({ song: 0, playing: false, name: '' });
  const [loop, setLoop] = useState(false);
  const [bpm, setBpm] = useState(120);
  const [songBpm, setSongBpm] = useState(120);            // tempo of the last song that played
  const [selVoice, setSelVoice] = useState('');
  const [selVoiceName, setSelVoiceName] = useState('');   // last-picked instrument name (shown on the card, persists across browsing)
  const [cart, setCart] = useState<{ rel: string; name: string } | null>(null);
  const [vpath, setVpath] = useState('');                 // dexed folder-browser current path ('' = root)
  const [level, setLevel] = useState<DirPage>(EMPTY_DIR); // current /dexed folder listing (lazy @DXLS)
  const [cartVoices, setCartVoices] = useState<string[]>([]); // open cart's 32 voice names (lazy @DXVL)
  const [libBusy, setLibBusy] = useState(false);          // a browse/voices fetch is in flight
  const [q, setQ] = useState({ voice: '', cart: '', groove: '' });
  const [busy, setBusy] = useState(false);

  // Hydrate every card from the device's real current settings (the @STATE reply). The
  // device only knows what's ACTIVE, so "selected but not playing" song/groove rows stay
  // as-is; everything the firmware tracks (vol/bpm/arp/loop/voice/kit/what's playing) is
  // restored. Sets state only — no echo back to the device.
  const clampIdx = (v: any, n: number) => Math.max(0, Math.min(n - 1, (v | 0)));
  function hydrate(j: any) {
    if (j.vol != null) setVol(j.vol);
    if (j.bpm != null) setBpm(j.bpm);
    if (j.loop != null) setLoop(!!j.loop);
    if (j.arp) setArp({ on: !!j.arp.on, pat: clampIdx(j.arp.pat, ARP_PAT.length), rate: clampIdx(j.arp.rate, ARP_RATE.length), oct: Math.max(1, Math.min(4, j.arp.oct | 0)) || 1, latch: !!j.arp.latch });
    if (j.song) setPlayer(p => ({ ...p, playing: !!j.song.playing, song: j.song.i | 0 }));
    if (j.drums) setDrums(d => ({ ...d, kit: j.drums.kit | 0, playing: j.drums.playing ? d.playing : null }));
    if (j.voice) {
      if (j.voice.cart) { setSelVoice('c' + j.voice.cart + ':' + (j.voice.cv | 0)); setSelVoiceName(j.voice.name || ''); }
      else if (j.voice.i != null && j.voice.i < 320) { setSelVoice('b' + (j.voice.i | 0)); setSelVoiceName(j.voice.name || ''); }
    }
  }

  useEffect(() => tp.onLine(line => {
    if (line.startsWith('@STATE=')) {
      try { hydrate(JSON.parse(line.slice(line.indexOf('=') + 1))); } catch {}
    } else if (line.indexOf('"conn"') >= 0 && line.indexOf('"vol"') >= 0) {
      const m = line.match(/\{.*\}/); if (m) { try { const j = JSON.parse(m[0]); setBt({ conn: !!j.conn, peer: j.peer || '' }); if (j.vol != null) setVol(j.vol); } catch {} }
    } else if (line.startsWith('[song]')) {
      // Follow the song's detected tempo: set master BPM to it (song + drums lock to that).
      const m = line.match(/([\d.]+)\s*bpm/); if (m) { const b = Math.round(parseFloat(m[1])); if (b >= 20 && b <= 300) { setSongBpm(b); setBpm(b); tp.masterBpm(b); } }
    }
  }), []);

  async function connect() {
    try {
      await tp.connect(); setConnected(true);
      await load();
      tp.requestState();   // pull the device's real current settings → hydrate every card (see @STATE handler)
    }
    catch (e: any) { notify('Connect failed: ' + e + (Platform.OS === 'web' ? '\n\nClose any control.html tab (one page owns the port), then retry.' : '')); }
  }
  async function disconnect() { try { await tp.disconnect(); } catch {} setConnected(false); setLoaded(false); setRoute('home'); }
  async function load() {
    try { setCat(await loadCatalog(tp)); setLoaded(true); }
    catch (e: any) {
      const yes = Platform.OS === 'web'
        ? (globalThis as any).confirm?.('Catalog load failed: ' + (e?.message || e) + '\n\nRebuild it now (@REINDEX)?')
        : true;
      if (yes) await reindex();
    }
  }
  async function reindex() { setBusy(true); try { await tp.reindex(); await load(); } finally { setBusy(false); } }

  const grooves = useMemo(() => { const t = q.groove.toLowerCase(); return cat.grooves.filter(g => !t || g.name.toLowerCase().includes(t)).slice(0, 500); }, [cat.grooves, q.groove]);

  // Lazy /dexed browse: fetch the current folder level via @DXLS whenever the path changes
  // (skip the synthetic '@bundled' view). A superseded reply is ignored via the `alive` gate.
  useEffect(() => {
    if (!loaded || vpath === '@bundled') { setLevel(EMPTY_DIR); return; }
    let alive = true;
    setLibBusy(true);
    tp.browseDir(vpath).then(d => { if (alive) setLevel(d); })
      .catch(() => { if (alive) setLevel({ ...EMPTY_DIR, path: vpath }); })
      .finally(() => { if (alive) setLibBusy(false); });
    return () => { alive = false; };
  }, [vpath, loaded]);

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
  const pickVoice = (it: VItem) => { setSelVoice(it.key); setSelVoiceName(it.label.replace(/^\d+\.\s*/, '')); if (it.key[0] === 'c' && cart) tp.dxPick(cart.rel, it.i); else if (it.key[0] === 'b') tp.dxVoice(it.i); };
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
      const v = cat.instruments[ni]; if (v) { setSelVoice('b' + v.i); setSelVoiceName(v.name); tp.dxVoice(v.i); }
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
  const stepSong = (dir: number) => {   // step the selected song; if one is playing, start the new one
    if (!cat.songs.length) return;
    const idx = cat.songs.findIndex(sg => sg.i === player.song);
    const ni = Math.max(0, Math.min(cat.songs.length - 1, (idx < 0 ? 0 : idx) + dir));
    const sg = cat.songs[ni]; if (!sg) return;
    setPlayer(p => { if (p.playing) { tp.playSong(sg.i); return { ...p, song: sg.i, name: sg.name }; } return { ...p, song: sg.i }; });
  };
  const stepBpm = (delta: number) => { const b = Math.max(20, Math.min(300, Math.round(bpm) + delta)); setBpm(b); tp.masterBpm(b); };
  const stepArpPat = (dir: number) => { const i = (arp.pat + dir + ARP_PAT.length) % ARP_PAT.length; setArp(a => ({ ...a, pat: i })); tp.arpPattern(i); };
  const playSong = () => { const sg = cat.songs.find(x => x.i === player.song); tp.playSong(player.song); setPlayer(p => ({ ...p, playing: true, name: sg?.name || '' })); };
  const stopSong = () => { tp.stopSong(); setPlayer(p => ({ ...p, playing: false })); };
  const playGroove = () => { const g = cat.grooves.find(x => x.path === drums.sel); if (g) { tp.playGrooveFile(grooveFile(g)); setDrums(d => ({ ...d, playing: g.name })); } };
  const stopDrums = () => { tp.stopDrums(); setDrums(d => ({ ...d, playing: null })); };

  const headerStatus = !connected ? 'Not connected' :
    [cat.engine || 'synth', cat.drumEngine ? cat.drumEngine + ' drums' : '', tp.name, bt.conn ? 'BT:' + (bt.peer || 'on') : '', drums.playing ? '♪ ' + drums.playing : ''].filter(Boolean).join('  ·  ');

  // ===== the sections: one entry drives both its homepage card and its page. =====
  // `value`/`status` = the subtitle; `actions` = the header controls; `body` = the page.
  type Section = { id: string; title: string; show: boolean; value?: string; status?: string; actions?: React.ReactNode; body: React.ReactNode; fullHeight?: boolean };

  // The card/page subtitle: the currently-loaded instrument if one is picked, else a
  // summary of where the browser is (folder name or catalog counts).
  const synthValue = selVoiceName
    || (vpath === '@bundled' ? 'Bundled' : vpath ? (vpath.split('/').pop() || '') : (cat.instruments.length ? cat.instruments.length + ' voices + SD library' : 'Library'));

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
              <Stat label="Soundfonts" n={cat.soundfonts.length} />
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
      id: 'bt', title: 'Bluetooth', show: true, status: bt.conn ? 'connected' + (bt.peer ? ': ' + bt.peer : '') : 'off',
      body: (
        <>
          <Text style={s.muted}>{bt.conn ? 'Connected: ' + (bt.peer || 'source') : 'No audio source connected'}</Text>
          <Row>
            <Pressable style={s.btn} onPress={() => tp.espPair()}><Text style={s.btnText}>Pairing mode</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost]} onPress={() => tp.espReconnect()}><Text style={s.btnText}>Reconnect</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost]} onPress={() => tp.espForget()}><Text style={s.btnText}>Forget</Text></Pressable>
          </Row>
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
        </>
      ),
    },
    // SYNTH / VOICES — folder browser over bundled voices + the whole /dexed library
    {
      id: 'synth', title: 'Synth / Voices', show: true, value: synthValue, fullHeight: true,
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
              {level.folders.map(f => <ListBtn key={'f' + f} label={'📁 ' + f} onPress={() => setVpath(vpath ? vpath + '/' + f : f)} />)}
              {level.carts.map(c => <ListBtn key={c.rel} label={'🎛 ' + c.name} onPress={() => setCart({ rel: c.rel, name: c.name })} />)}
              {level.folders.length === 0 && level.carts.length === 0 && vpath !== '' && <Text style={s.muted}>(empty folder)</Text>}
            </ScrollView>
          )}
        </View>
      ),
    },
    // MIDI PLAYER — select a song; Play/Stop in the header (works collapsed); loop toggle
    {
      id: 'player', title: 'MIDI Player', show: true,
      value: (player.playing ? '♪ ' : '') + (cat.songs.find(sg => sg.i === player.song)?.name || '—'),
      actions: (<>
        <HdrBtn label="‹" stop onPress={() => stepSong(-1)} />
        <HdrBtn label="›" stop onPress={() => stepSong(1)} />
        <HdrBtn label="▶" onPress={playSong} />
        <HdrBtn label="■" stop onPress={stopSong} />
      </>),
      body: (
        <>
          {cat.songs.length === 0 ? <Text style={s.muted}>No songs indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {cat.songs.map(sg => <ListBtn key={sg.i} label={(player.playing && player.song === sg.i ? '♪ ' : '') + sg.name} sel={player.song === sg.i}
                onPress={() => setPlayer(p => ({ ...p, song: sg.i }))} />)}
            </ScrollView>
          )}
          <Row><Text style={[s.muted, { flex: 1 }]}>Loop</Text>
            <Switch value={loop} onValueChange={v => { setLoop(v); tp.songLoop(v); }} /></Row>
        </>
      ),
    },
    // ARPEGGIATOR
    {
      id: 'arp', title: 'Arpeggiator', show: true, value: (arp.on ? '' : '(off)  ') + ARP_PAT[arp.pat] + '  ·  ' + ARP_RATE[arp.rate],
      actions: (<>
        <HdrBtn label="‹" stop onPress={() => stepArpPat(-1)} />
        <HdrBtn label="›" stop onPress={() => stepArpPat(1)} />
        <Switch value={arp.on} onValueChange={v => { setArp(a => ({ ...a, on: v })); tp.arpOn(v); }} style={{ marginLeft: 6 }} />
      </>),
      body: (
        <>
          <Row><Text style={[s.muted, { flex: 1 }]}>Enabled</Text>
            <Switch value={arp.on} onValueChange={v => { setArp(a => ({ ...a, on: v })); tp.arpOn(v); }} /></Row>
          <Row><Text style={[s.muted, { flex: 1 }]}>Pattern</Text>
            {ARP_PAT.map((p, i) => <Pressable key={i} style={[s.pill, arp.pat === i && s.pillOn]} onPress={() => { setArp(a => ({ ...a, pat: i })); tp.arpPattern(i); }}><Text style={s.text}>{p}</Text></Pressable>)}</Row>
          <Row><Text style={[s.muted, { flex: 1 }]}>Rate</Text>
            {ARP_RATE.map((r, i) => <Pressable key={i} style={[s.pill, arp.rate === i && s.pillOn]} onPress={() => { setArp(a => ({ ...a, rate: i })); tp.arpRate(i); }}><Text style={s.text}>{r}</Text></Pressable>)}</Row>
          <Row><Text style={[s.muted, { flex: 1 }]}>Octaves {arp.oct}</Text>
            {[1, 2, 3, 4].map(n => <Pressable key={n} style={[s.pill, arp.oct === n && s.pillOn]} onPress={() => { setArp(a => ({ ...a, oct: n })); tp.arpOctaves(n); }}><Text style={s.text}>{n}</Text></Pressable>)}</Row>
          <Row><Text style={[s.muted, { flex: 1 }]}>Latch</Text>
            <Switch value={arp.latch} onValueChange={v => { setArp(a => ({ ...a, latch: v })); tp.arpLatch(v); }} /></Row>
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
    // TAC5212
    {
      id: 'codec', title: 'TAC5212', show: true,
      body: <Text style={s.muted}>Codec routing + trims. (Controls added as the command set lands.) Master output is the header volume.</Text>,
    },
  ];

  const visible = sections.filter(x => x.show);
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
          <Pressable style={s.btn} onPress={() => (connected ? disconnect() : connect())}>
            <Text style={s.btnText}>{connected ? 'Disconnect' : `${tp.name} • Connect`}</Text>
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

      {!connected && <Text style={[s.muted, { textAlign: 'center', marginTop: 48 }]}>Connect a T-DSP device to begin.</Text>}

      {connected && (
        <View style={{ flex: 1 }}>
          {/* ===== HOMEPAGE: a responsive grid of section cards ===== */}
          {!cur && (
            <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 400 }}>
              <View style={s.home}>
                {visible.map(sec => (
                  <View key={sec.id} style={[s.cell, { width: `${100 / cols}%` }]}>
                    <Card title={sec.title} value={sec.value} status={sec.status} actions={sec.actions}
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
                <PageHeader title={cur.title} value={cur.value} status={cur.status} actions={cur.actions} onBack={() => setRoute('home')} />
                <View style={s.pageBody}>{cur.body}</View>
              </View>
            </ScrollView>
          )}

          {/* ===== full-height pages: stay MOUNTED (just hidden when inactive) so the
                  selected folder AND the picker's scroll position persist across nav ===== */}
          {fullPages.map(sec => (
            <View key={sec.id} style={[s.page, { flex: 1 }, route !== sec.id && s.hidden]}>
              <PageHeader title={sec.title} value={sec.value} status={sec.status} actions={sec.actions} onBack={() => setRoute('home')} />
              <View style={[s.pageBody, { flex: 1 }]}>{sec.body}</View>
            </View>
          ))}
        </View>
      )}
    </View>
  );
}

const s = StyleSheet.create({
  app: { flex: 1, backgroundColor: C.bg, paddingTop: Platform.OS === 'web' ? 0 : 34 },
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
  tag: { color: C.muted, fontSize: 12, backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 2, borderRadius: 10, overflow: 'hidden', alignSelf: 'flex-start' },
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
  text: { color: C.text, fontSize: 14 },
  btn: { backgroundColor: '#238636', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 7, alignItems: 'center' },
  btnWide: { marginTop: 4 },
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
  pill: { backgroundColor: C.chip, paddingHorizontal: 12, paddingVertical: 6, borderRadius: 14 },
  pillOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  statGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  stat: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 8, paddingVertical: 10, paddingHorizontal: 12, minWidth: 96, flexGrow: 1, alignItems: 'center' },
  statN: { color: C.text, fontSize: 22, fontWeight: '800' },
  statL: { color: C.muted, fontSize: 12, marginTop: 2 },
  statSub: { color: C.accent, fontSize: 11, marginTop: 1 },
});
