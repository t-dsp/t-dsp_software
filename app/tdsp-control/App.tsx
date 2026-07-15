// App.tsx — T-DSP unified control surface (react-native-web on desktop, native in the app).
// One React/TS codebase over a platform-split transport (Web Serial / BLE). The on-device
// catalog DB (/tdsp/*.ndjson, built by @REINDEX) is the source of truth; browsing is local,
// only actions hit the wire. Old single-file UI preserved as App.old.tsx.
import React, { useState, useRef, useEffect, useMemo } from 'react';
import { View, Text, Pressable, ScrollView, TextInput, Switch, StyleSheet, ActivityIndicator, Platform, Alert } from 'react-native';
import Slider from '@react-native-community/slider';
import { createTransport } from './src/transportFactory';
import { Catalog, EMPTY_CATALOG, loadCatalog, cartRel, Cart } from './src/catalog';
import type { Transport } from './src/transport';

const C = { bg: '#0d1117', card: '#161b22', card2: '#0e131a', border: '#30363d', text: '#e6edf3', muted: '#8b949e', accent: '#3fb950', sel: 'rgba(31,111,235,0.28)', chip: '#21262d' };
const ARP_PAT = ['Up', 'Down', 'Up/Down', 'Random'];
const ARP_RATE = ['1/4', '1/8', '1/8T', '1/16', '1/16T', '1/32'];

function notify(msg: string) { if (Platform.OS === 'web') (globalThis as any).alert?.(msg); else Alert.alert('T-DSP', msg); }

function Accordion({ title, status, open, onPress, children }:
  { title: string; status?: string; open: boolean; onPress: () => void; children?: React.ReactNode }) {
  return (
    <View style={s.card}>
      <Pressable style={s.drawer} onPress={onPress}>
        <Text style={s.drawerTitle}>{title}</Text>
        {!!status && <Text style={s.tag}>{status}</Text>}
        <Text style={[s.chev, open && s.chevOpen]}>›</Text>
      </Pressable>
      {open && <View style={s.body}>{children}</View>}
    </View>
  );
}
const Row = ({ children }: any) => <View style={s.row}>{children}</View>;
const Stat = ({ label, n, sub }: { label: string; n: number; sub?: string }) => (
  <View style={s.stat}><Text style={s.statN}>{n}</Text><Text style={s.statL}>{label}</Text>{!!sub && <Text style={s.statSub}>{sub}</Text>}</View>
);
const ListBtn = ({ label, sel, onPress }: any) => (
  <Pressable onPress={onPress} style={[s.listBtn, sel && s.listBtnSel]}><Text style={s.text} numberOfLines={1}>{label}</Text></Pressable>
);

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
  const [player, setPlayer] = useState({ song: 0, playing: false });
  const [selVoice, setSelVoice] = useState('');
  const [cart, setCart] = useState<Cart | null>(null);
  const [q, setQ] = useState({ voice: '', cart: '', groove: '' });
  const [busy, setBusy] = useState(false);

  useEffect(() => tp.onLine(line => {
    if (line.indexOf('"conn"') >= 0 && line.indexOf('"vol"') >= 0) {
      const m = line.match(/\{.*\}/); if (m) { try { const j = JSON.parse(m[0]); setBt({ conn: !!j.conn, peer: j.peer || '' }); if (j.vol != null) setVol(j.vol); } catch {} }
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

        {/* SYNTH / VOICES */}
        <Accordion title="Synth / Voices" status={selVoice ? 'voice set' : ''} open={openId === 'synth'} onPress={() => toggle('synth')}>
          {!loaded ? <Text style={s.muted}>Connect to load voices.</Text> : (
            <>
              <TextInput style={s.input} placeholder="Search bundled voices…" placeholderTextColor={C.muted}
                value={q.voice} onChangeText={t => setQ(x => ({ ...x, voice: t }))} />
              <ScrollView style={s.list} nestedScrollEnabled>
                {bundled.map(v => <ListBtn key={'b' + v.i} label={v.name} sel={selVoice === 'b' + v.i}
                  onPress={() => { setSelVoice('b' + v.i); tp.dxVoice(v.i); }} />)}
              </ScrollView>
              {cat.dexed.length > 0 && (
                <Accordion title="Cart library" status={cat.dexed.length + ' carts'} open={openId === 'carts'} onPress={() => toggle('carts')}>
                  <TextInput style={s.input} placeholder="Filter carts…" placeholderTextColor={C.muted}
                    value={q.cart} onChangeText={t => setQ(x => ({ ...x, cart: t }))} />
                  <ScrollView style={s.list} nestedScrollEnabled>
                    {carts.map(c => <ListBtn key={c.path} label={(c.folder ? c.folder + ' / ' : '') + c.name}
                      sel={cart?.path === c.path} onPress={() => setCart(c)} />)}
                  </ScrollView>
                  {cart && <>
                    <Text style={s.muted}>{cart.voices.length} voices in {cart.name}</Text>
                    <ScrollView style={s.list} nestedScrollEnabled>
                      {cart.voices.map((vn, i) => <ListBtn key={i} label={(i + 1) + '. ' + vn} sel={selVoice === 'c' + cart.path + ':' + i}
                        onPress={() => { setSelVoice('c' + cart!.path + ':' + i); tp.dxPick(cartRel(cart!), i); }} />)}
                    </ScrollView>
                  </>}
                </Accordion>
              )}
            </>
          )}
        </Accordion>

        {/* MIDI PLAYER — select a song, then Play / Stop */}
        <Accordion title="MIDI Player" status={player.playing ? 'playing' : ''} open={openId === 'player'} onPress={() => toggle('player')}>
          {cat.songs.length === 0 ? <Text style={s.muted}>No songs indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {cat.songs.map(sg => <ListBtn key={sg.i} label={sg.name} sel={player.song === sg.i}
                onPress={() => setPlayer(p => ({ ...p, song: sg.i }))} />)}
            </ScrollView>
          )}
          <Row>
            <Pressable style={[s.btn, s.grow1]} onPress={() => { tp.playSong(player.song); setPlayer(p => ({ ...p, playing: true })); }}><Text style={s.btnText}>▶ Play</Text></Pressable>
            <Pressable style={[s.btn, s.btnGhost, s.grow1]} onPress={() => { tp.stopSong(); setPlayer(p => ({ ...p, playing: false })); }}><Text style={s.btnText}>■ Stop</Text></Pressable>
          </Row>
        </Accordion>

        {/* ARPEGGIATOR */}
        <Accordion title="Arpeggiator" status={arp.on ? 'on' : ''} open={openId === 'arp'} onPress={() => toggle('arp')}>
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
        <Accordion title="Drums" status={drums.playing || ''} open={openId === 'drums'} onPress={() => toggle('drums')}>
          <Row><Text style={[s.muted, { flex: 1 }]}>Kit</Text>
            <ScrollView horizontal showsHorizontalScrollIndicator={false}>
              {cat.drumkits.map((k, i) => <Pressable key={i} style={[s.pill, drums.kit === i && s.pillOn]} onPress={() => { setDrums(d => ({ ...d, kit: i })); tp.drumKit(i); }}><Text style={s.text}>{k.name}</Text></Pressable>)}
            </ScrollView></Row>
          <TextInput style={s.input} placeholder="Search grooves…" placeholderTextColor={C.muted}
            value={q.groove} onChangeText={t => setQ(x => ({ ...x, groove: t }))} />
          {cat.grooves.length === 0 ? <Text style={s.muted}>No grooves indexed.</Text> : (
            <ScrollView style={s.list} nestedScrollEnabled>
              {grooves.map(g => <ListBtn key={g.path} label={(drums.playing === g.name ? '♪ ' : '') + g.name} sel={drums.sel === g.name}
                onPress={() => setDrums(d => ({ ...d, sel: g.name }))} />)}
            </ScrollView>
          )}
          <Row>
            <Pressable style={[s.btn, s.grow1]} disabled={!drums.sel}
              onPress={() => { if (drums.sel) { tp.playGrooveFile(drums.sel); setDrums(d => ({ ...d, playing: d.sel })); } }}><Text style={s.btnText}>▶ Play</Text></Pressable>
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
