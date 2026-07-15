// App.tsx — T-DSP unified control surface (react-native-web on desktop, native in the app).
// One React/TS codebase over a platform-split transport (Web Serial / BLE). The on-device
// catalog DB (/tdsp/*.ndjson, built by @REINDEX) is the source of truth; browsing is local,
// only actions hit the wire. Old single-file UI preserved as App.old.tsx.
import React, { useState, useRef, useEffect, useMemo } from 'react';
import { View, Text, Pressable, ScrollView, FlatList, TextInput, Switch, StyleSheet, ActivityIndicator, Platform, Alert } from 'react-native';
import Slider from '@react-native-community/slider';
import { createTransport } from './src/transportFactory';
import { Catalog, EMPTY_CATALOG, loadCatalog, cartRel, Cart } from './src/catalog';
import type { Transport } from './src/transport';

// One level of the /dexed folder tree: subfolders + carts sitting directly at `path`.
function dexedLevel(dexed: Cart[], path: string) {
  const prefix = path ? path + '/' : '';
  const folders = new Set<string>();
  const carts: Cart[] = [];
  for (const c of dexed) {
    const f = c.folder || '';
    if (f === path) { carts.push(c); continue; }
    if (f.startsWith(prefix)) { const seg = f.slice(prefix.length).split('/')[0]; if (seg) folders.add(seg); }
  }
  return { folders: [...folders].sort((a, b) => a.localeCompare(b)), carts: carts.sort((a, b) => a.name.localeCompare(b.name)) };
}
const grooveFile = (g: { path: string; name: string }) => g.path.split('/').pop() || (g.name + '.mid');   // @DRUMF wants filename WITH .mid

const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };
const ARP_PAT = ['Up', 'Down', 'Up/Down', 'Random'];
const ARP_RATE = ['1/4', '1/8', '1/8T', '1/16', '1/16T', '1/32'];

function notify(msg: string) { if (Platform.OS === 'web') (globalThis as any).alert?.(msg); else Alert.alert('T-DSP', msg); }

function Accordion({ title, status, open, onPress, headerActions, children }:
  { title: string; status?: string; open: boolean; onPress: () => void; headerActions?: React.ReactNode; children?: React.ReactNode }) {
  return (
    <View style={s.card}>
      <Pressable style={s.drawer} onPress={onPress}>
        <Text style={s.drawerTitle}>{title}</Text>
        {!!status && <Text style={s.tag}>{status}</Text>}
        <View style={{ flex: 1 }} />
        {headerActions}
        <Text style={[s.chev, open && s.chevOpen]}>›</Text>
      </Pressable>
      {open && <View style={s.body}>{children}</View>}
    </View>
  );
}
// A compact transport button for an accordion header (nested Pressable → doesn't toggle the drawer).
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
  const [connected, setConnected] = useState(false);
  const [cat, setCat] = useState<Catalog>(EMPTY_CATALOG);
  const [loaded, setLoaded] = useState(false);
  const [openId, setOpenId] = useState<string>('conn');
  const [vol, setVol] = useState(80);
  const [bt, setBt] = useState({ conn: false, peer: '' });
  const [arp, setArp] = useState({ on: false, pat: 0, rate: 0, oct: 1, latch: false });
  const [drums, setDrums] = useState<{ kit: number; sel: string | null; playing: string | null }>({ kit: 0, sel: null, playing: null });
  const [player, setPlayer] = useState<{ song: number; playing: boolean; name: string }>({ song: 0, playing: false, name: '' });
  const [loop, setLoop] = useState(false);
  const [bpm, setBpm] = useState(120);
  const [songBpm, setSongBpm] = useState(120);            // tempo of the last song that played
  const [selVoice, setSelVoice] = useState('');
  const [cart, setCart] = useState<Cart | null>(null);
  const [vpath, setVpath] = useState('');                 // dexed folder-browser current path ('' = root)
  const [q, setQ] = useState({ voice: '', cart: '', groove: '' });
  const [busy, setBusy] = useState(false);

  useEffect(() => tp.onLine(line => {
    if (line.indexOf('"conn"') >= 0 && line.indexOf('"vol"') >= 0) {
      const m = line.match(/\{.*\}/); if (m) { try { const j = JSON.parse(m[0]); setBt({ conn: !!j.conn, peer: j.peer || '' }); if (j.vol != null) setVol(j.vol); } catch {} }
    } else if (line.startsWith('[song]')) {
      // Follow the song's detected tempo: set master BPM to it (song + drums lock to that).
      const m = line.match(/([\d.]+)\s*bpm/); if (m) { const b = Math.round(parseFloat(m[1])); if (b >= 20 && b <= 300) { setSongBpm(b); setBpm(b); tp.masterBpm(b); } }
    }
  }), []);

  async function connect() {
    try {
      await tp.connect(); setConnected(true);
      tp.arpOn(false); setArp(a => ({ ...a, on: false }));   // arp OFF by default (sync device to UI)
      await load();
    }
    catch (e: any) { notify('Connect failed: ' + e + (Platform.OS === 'web' ? '\n\nClose any control.html tab (one page owns the port), then retry.' : '')); }
  }
  async function disconnect() { try { await tp.disconnect(); } catch {} setConnected(false); setLoaded(false); }
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
  const toggle = (id: string) => setOpenId(o => (o === id ? '' : id));

  const dexedVoices = useMemo(() => cat.dexed.reduce((n, c) => n + (c.voices?.length || 0), 0), [cat.dexed]);
  const bundled = useMemo(() => { const t = q.voice.toLowerCase(); return cat.instruments.filter(v => !t || (v.name || '').toLowerCase().includes(t)).slice(0, 400); }, [cat.instruments, q.voice]);
  const carts = useMemo(() => { const t = q.cart.toLowerCase(); return cat.dexed.filter(c => !t || (c.name + ' ' + (c.folder || '')).toLowerCase().includes(t)).slice(0, 500); }, [cat.dexed, q.cart]);
  const grooves = useMemo(() => { const t = q.groove.toLowerCase(); return cat.grooves.filter(g => !t || g.name.toLowerCase().includes(t)).slice(0, 500); }, [cat.grooves, q.groove]);
  const level = useMemo(() => dexedLevel(cat.dexed, vpath), [cat.dexed, vpath]);

  // The voices currently listed in Synth/Voices (a cart's voices, or the bundled set).
  const voiceRef = useRef<FlatList<VItem>>(null);
  const voiceData: VItem[] = useMemo(() =>
    cart ? cart.voices.map((vn, i) => ({ key: 'c' + cart.path + ':' + i, label: (i + 1) + '. ' + vn, i }))
      : vpath === '@bundled' ? cat.instruments.map(v => ({ key: 'b' + v.i, label: v.name, i: v.i }))
        : [], [cart, vpath, cat.instruments]);
  const pickVoice = (it: VItem) => { setSelVoice(it.key); if (it.key[0] === 'c' && cart) tp.dxPick(cartRel(cart), it.i); else if (it.key[0] === 'b') tp.dxVoice(it.i); };
  const stepVoice = (dir: number) => { if (!voiceData.length) return; const idx = voiceData.findIndex(d => d.key === selVoice); const ni = Math.max(0, Math.min(voiceData.length - 1, (idx < 0 ? 0 : idx) + dir)); if (voiceData[ni]) pickVoice(voiceData[ni]); };
  useEffect(() => {   // keep the selected voice in view (on pick + on list change)
    const idx = voiceData.findIndex(d => d.key === selVoice);
    if (idx >= 0) { const t = setTimeout(() => { try { voiceRef.current?.scrollToIndex({ index: idx, animated: true, viewPosition: 0.5 }); } catch {} }, 60); return () => clearTimeout(t); }
  }, [selVoice, voiceData]);

  const stepGroove = (dir: number) => { if (!grooves.length) return; const idx = grooves.findIndex(g => g.path === drums.sel); const ni = Math.max(0, Math.min(grooves.length - 1, (idx < 0 ? 0 : idx) + dir)); const g = grooves[ni]; if (g) setDrums(d => ({ ...d, sel: g.path })); };
  const stepBpm = (delta: number) => { const b = Math.max(20, Math.min(300, Math.round(bpm) + delta)); setBpm(b); tp.masterBpm(b); };
  const stepArpPat = (dir: number) => { const i = (arp.pat + dir + ARP_PAT.length) % ARP_PAT.length; setArp(a => ({ ...a, pat: i })); tp.arpPattern(i); };

  const headerStatus = !connected ? 'Not connected' :
    [cat.engine || 'synth', cat.drumEngine ? cat.drumEngine + ' drums' : '', tp.name, bt.conn ? 'BT:' + (bt.peer || 'on') : '', drums.playing ? '♪ ' + drums.playing : ''].filter(Boolean).join('  ·  ');

  return (
    <View style={s.app}>
      {/* ===== header: brand, connect, master volume ===== */}
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

      <ScrollView style={{ flex: 1 }} contentContainerStyle={{ paddingBottom: 40 }}>
        {!connected && <Text style={[s.muted, { textAlign: 'center', marginTop: 48 }]}>Connect a T-DSP device to begin.</Text>}
        {connected && <>
        {/* CONNECTION — catalog stats */}
        <Accordion title="Connection" status={cat.engine || (connected ? 'connected' : '')} open={openId === 'conn'} onPress={() => toggle('conn')}>
          <Text style={s.muted}>Synth: <Text style={s.text}>{cat.engine || '—'}</Text>   ·   Transport: {tp.name}</Text>
          {!loaded && <Text style={s.muted}>{connected ? 'Loading catalog…' : 'Connect to load the catalog.'}</Text>}
          {loaded && (
            <View style={s.statGrid}>
              <Stat label="Instruments" n={cat.instruments.length} />
              <Stat label="Dexed carts" n={cat.dexed.length} sub={dexedVoices ? dexedVoices + ' voices' : undefined} />
              <Stat label="Grooves" n={cat.grooves.length} />
              <Stat label="Songs" n={cat.songs.length} />
              <Stat label="Soundfonts" n={cat.soundfonts.length} />
              <Stat label="Drum kits" n={cat.drumkits.length} />
            </View>
          )}
          <Pressable style={[s.btn, s.btnWide]} onPress={reindex} disabled={!connected || busy}>
            {busy ? <ActivityIndicator color={C.text} /> : <Text style={s.btnText}>Rebuild catalog (@REINDEX)</Text>}
          </Pressable>
        </Accordion>

        {/* BLUETOOTH */}
        <Accordion title="Bluetooth" status={bt.conn ? 'connected' + (bt.peer ? ': ' + bt.peer : '') : 'off'} open={openId === 'bt'} onPress={() => toggle('bt')}>
          <Text style={s.muted}>{bt.conn ? 'Connected: ' + (bt.peer || 'source') : 'No audio source connected'}</Text>
          <Row>
            <Pressable style={s.btn} onPress={() => tp.espPair()}><Text style={s.btnText}>Pairing mode</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost]} onPress={() => tp.espReconnect()}><Text style={s.btnText}>Reconnect</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost]} onPress={() => tp.espForget()}><Text style={s.btnText}>Forget</Text></Pressable>
          </Row>
        </Accordion>

        {/* TEMPO / BPM — master tempo; song + drums lock to it (@BPM=) */}
        <Accordion title="Tempo" status={Math.round(bpm) + ' BPM'} open={openId === 'bpm'} onPress={() => toggle('bpm')}
          headerActions={<>
            <HdrBtn label="−" stop onPress={() => stepBpm(-1)} />
            <HdrBtn label="＋" onPress={() => stepBpm(1)} />
          </>}>
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
        </Accordion>

        {/* SYNTH / VOICES — folder browser over bundled voices + the whole /dexed library */}
        <Accordion title="Synth / Voices"
          status={cart ? cart.name : vpath === '@bundled' ? 'Bundled' : vpath ? (vpath.split('/').pop() || '') : (cat.dexed.length ? cat.dexed.length + ' carts' : cat.instruments.length + ' voices')}
          open={openId === 'synth'} onPress={() => toggle('synth')}
          headerActions={voiceData.length ? <>
            <HdrBtn label="‹" stop onPress={() => stepVoice(-1)} />
            <HdrBtn label="›" stop onPress={() => stepVoice(1)} />
          </> : undefined}>
          {!loaded ? <Text style={s.muted}>Connect to load voices.</Text> : (
            <>
              {(cart || vpath) ? (
                <Pressable style={[s.btn, s.btnGhost, { alignSelf: 'flex-start' }]} onPress={() => {
                  if (cart) setCart(null);
                  else if (vpath === '@bundled') setVpath('');
                  else setVpath(vpath.split('/').slice(0, -1).join('/'));
                }}><Text style={s.btnText}>‹ Back</Text></Pressable>
              ) : null}
              {voiceData.length ? (
                <>
                  <Text style={s.muted}>{cart ? cart.name + ' — ' + cart.voices.length + ' voices' : 'Bundled voices (' + cat.instruments.length + ')'}</Text>
                  <FlatList ref={voiceRef} data={voiceData} style={s.list} nestedScrollEnabled keyExtractor={d => d.key}
                    getItemLayout={(_, index) => ({ length: ROW_H, offset: ROW_H * index, index })}
                    onScrollToIndexFailed={() => {}}
                    renderItem={({ item }) => <ListBtn label={item.label} sel={selVoice === item.key} onPress={() => pickVoice(item)} />} />
                </>
              ) : (
                <ScrollView style={s.list} nestedScrollEnabled>
                  {vpath === '' && <ListBtn label={'★ Bundled voices (' + cat.instruments.length + ')'} onPress={() => setVpath('@bundled')} />}
                  {level.folders.map(f => <ListBtn key={'f' + f} label={'📁 ' + f} onPress={() => setVpath(vpath ? vpath + '/' + f : f)} />)}
                  {level.carts.map(c => <ListBtn key={c.path} label={'🎛 ' + c.name} onPress={() => setCart(c)} />)}
                  {level.folders.length === 0 && level.carts.length === 0 && vpath !== '' && <Text style={s.muted}>(empty folder)</Text>}
                </ScrollView>
              )}
            </>
          )}
        </Accordion>

        {/* MIDI PLAYER — select a song; Play/Stop in the header (works collapsed); loop toggle */}
        <Accordion title="MIDI Player"
          status={player.playing ? '♪ ' + (player.name || cat.songs.find(sg => sg.i === player.song)?.name || 'playing') : ''}
          open={openId === 'player'} onPress={() => toggle('player')}
          headerActions={<>
            <HdrBtn label="▶" onPress={() => { const sg = cat.songs.find(x => x.i === player.song); tp.playSong(player.song); setPlayer(p => ({ ...p, playing: true, name: sg?.name || '' })); }} />
            <HdrBtn label="■" stop onPress={() => { tp.stopSong(); setPlayer(p => ({ ...p, playing: false })); }} />
          </>}>
          {cat.songs.length === 0 ? <Text style={s.muted}>No songs indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {cat.songs.map(sg => <ListBtn key={sg.i} label={(player.playing && player.song === sg.i ? '♪ ' : '') + sg.name} sel={player.song === sg.i}
                onPress={() => setPlayer(p => ({ ...p, song: sg.i }))} />)}
            </ScrollView>
          )}
          <Row><Text style={[s.muted, { flex: 1 }]}>Loop</Text>
            <Switch value={loop} onValueChange={v => { setLoop(v); tp.songLoop(v); }} /></Row>
        </Accordion>

        {/* ARPEGGIATOR */}
        <Accordion title="Arpeggiator" status={arp.on ? ('on · ' + ARP_PAT[arp.pat] + ' · ' + ARP_RATE[arp.rate]) : 'off'} open={openId === 'arp'} onPress={() => toggle('arp')}
          headerActions={<>
            <HdrBtn label="‹" stop onPress={() => stepArpPat(-1)} />
            <HdrBtn label="›" stop onPress={() => stepArpPat(1)} />
            <Switch value={arp.on} onValueChange={v => { setArp(a => ({ ...a, on: v })); tp.arpOn(v); }} style={{ marginLeft: 6 }} />
          </>}>
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
        </Accordion>

        {/* DRUMS — only if the built engine renders ch10 drums (hidden e.g. on a no-PSRAM TSF build) */}
        {cat.hasDrums && (
        <Accordion title="Drums"
          status={drums.playing ? '♪ ' + drums.playing : (cat.drumkits[drums.kit]?.name || '')}
          open={openId === 'drums'} onPress={() => toggle('drums')}
          headerActions={<>
            <HdrBtn label="‹" stop onPress={() => stepGroove(-1)} />
            <HdrBtn label="›" stop onPress={() => stepGroove(1)} />
            <HdrBtn label="▶" onPress={() => { const g = cat.grooves.find(x => x.path === drums.sel); if (g) { tp.playGrooveFile(grooveFile(g)); setDrums(d => ({ ...d, playing: g.name })); } }} />
            <HdrBtn label="■" stop onPress={() => { tp.stopDrums(); setDrums(d => ({ ...d, playing: null })); }} />
          </>}>
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
            <Pressable style={[s.btn, s.grow1]} disabled={!drums.sel}
              onPress={() => { const g = cat.grooves.find(x => x.path === drums.sel); if (g) { tp.playGrooveFile(grooveFile(g)); setDrums(d => ({ ...d, playing: g.name })); } }}><Text style={s.btnText}>▶ Play</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={() => { tp.stopDrums(); setDrums(d => ({ ...d, playing: null })); }}><Text style={s.btnText}>■ Stop</Text></Pressable>
          </Row>
        </Accordion>
        )}

        {/* TAC5212 */}
        <Accordion title="TAC5212" open={openId === 'codec'} onPress={() => toggle('codec')}>
          <Text style={s.muted}>Codec routing + trims. (Controls added as the command set lands.) Master output is the header volume.</Text>
        </Accordion>
        </>}
      </ScrollView>
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
  card: { backgroundColor: C.card, borderWidth: 1, borderColor: C.border, borderRadius: 10, marginHorizontal: 10, marginTop: 8, overflow: 'hidden' },
  drawer: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 14, paddingVertical: 13 },
  drawerTitle: { color: C.text, fontWeight: '600', fontSize: 15 },
  tag: { color: C.muted, fontSize: 12, backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 2, borderRadius: 10, overflow: 'hidden' },
  chev: { color: C.muted, fontSize: 20, marginLeft: 'auto' },
  chevOpen: { transform: [{ rotate: '90deg' }] },
  body: { paddingHorizontal: 14, paddingBottom: 14, gap: 8 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 6, flexWrap: 'wrap' },
  muted: { color: C.muted, fontSize: 13 },
  text: { color: C.text, fontSize: 14 },
  btn: { backgroundColor: '#238636', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 7, alignItems: 'center' },
  btnWide: { marginTop: 4 },
  grow1: { flex: 1 },
  hdrBtn: { backgroundColor: '#238636', paddingHorizontal: 11, paddingVertical: 4, borderRadius: 6, marginLeft: 4 },
  hdrBtnStop: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  hdrBtnText: { color: C.text, fontSize: 14, fontWeight: '700' },
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
