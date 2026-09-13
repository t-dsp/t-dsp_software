// DeviceWifi.tsx — Settings › Device Wi-Fi. Manage the networks the T-DSP joins and its own
// network's name/password, at runtime (no reflash). Talks HTTP to the ESP32 (src/deviceWifi.ts).
//
// Only meaningful on the WiFi control build, i.e. when the app is connected over Wi-Fi: `base` is
// the device's HTTP origin then, and null otherwise (USB / Bluetooth), which renders a short note.
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { View, Text, Pressable, TextInput, ActivityIndicator } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { C } from './theme';
import { s } from './styles';
import { Row } from './primitives';
import { wifiApi, staSummary, ScanNet, WifiStatus } from '../deviceWifi';

const PASS_KEY = 'tdsp.devicePassword';
const POLL_MS = 3000;

const Note = ({ children, warn }: { children: any; warn?: boolean }) => (
  <Text style={[s.muted, warn && { color: '#d29922' }]}>{children}</Text>
);

const Btn = ({ label, onPress, ghost, disabled }: { label: string; onPress: () => void; ghost?: boolean; disabled?: boolean }) => (
  <Pressable style={[s.btn, ghost && s.btnGhost, disabled && { opacity: 0.5 }]} onPress={onPress} disabled={disabled}>
    <Text style={s.btnText}>{label}</Text>
  </Pressable>
);

const bars = (rssi: number) => (rssi >= -60 ? '▂▄▆█' : rssi >= -70 ? '▂▄▆' : rssi >= -80 ? '▂▄' : '▂');

export default function DeviceWifi({ base }: { base: string | null }) {
  const api = base ? wifiApi(base) : null;
  const [st, setSt] = useState<WifiStatus | null>(null);
  const [err, setErr] = useState('');
  const [msg, setMsg] = useState('');
  const [busy, setBusy] = useState(false);
  const [auth, setAuth] = useState('');
  const [scan, setScan] = useState<ScanNet[] | null>(null);
  const [scanning, setScanning] = useState(false);
  const [ssid, setSsid] = useState('');
  const [pass, setPass] = useState('');
  const [apOpen, setApOpen] = useState(false);
  const [apSsid, setApSsid] = useState('');
  const [apPass, setApPass] = useState('');
  const [confirmForget, setConfirmForget] = useState<string | null>(null);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    AsyncStorage.getItem(PASS_KEY).then(v => { if (v && alive.current) setAuth(v); }).catch(() => {});
    return () => { alive.current = false; };
  }, []);

  const refresh = useCallback(async () => {
    if (!api) return;
    try {
      const j = await api.status();
      if (!alive.current) return;
      setSt(j);
      setErr('');
    } catch (e: any) {
      if (alive.current) setErr('Device settings did not answer (' + (e?.message || e) + ')');
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [base]);

  useEffect(() => {
    if (!api) return;
    refresh();
    const t = setInterval(refresh, POLL_MS);
    return () => clearInterval(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [base]);

  if (!api) {
    return <Note>Device Wi-Fi settings are available when the app is connected over Wi-Fi.</Note>;
  }

  const rememberAuth = (v: string) => { setAuth(v); AsyncStorage.setItem(PASS_KEY, v).catch(() => {}); };

  // Run a mutating call, show its outcome, refresh status.
  const act = async (label: string, fn: () => Promise<{ ok: boolean; error?: string }>, okMsg: string) => {
    if (!auth) { setMsg('Enter the device password first.'); return false; }
    setBusy(true); setMsg(label + '…');
    const r = await fn();
    if (!alive.current) return r.ok;
    setBusy(false);
    setMsg(r.ok ? okMsg : 'Could not ' + label.toLowerCase() + ': ' + (r.error || 'failed'));
    refresh();
    return r.ok;
  };

  const doScan = async () => {
    if (!auth) { setMsg('Enter the device password first.'); return; }
    setScanning(true); setMsg('');
    const r = await api.startScan(auth);
    if (!r.ok) { setScanning(false); setMsg('Could not scan: ' + (r.error || 'failed')); return; }
    // The device scans in the background (a few seconds in AP+STA); poll for the results.
    for (let i = 0; i < 12 && alive.current; i++) {
      await new Promise(res => setTimeout(res, 1000));
      try {
        const j = await api.scanResults();
        if (j.state === 'done') { setScan(j.results); break; }
      } catch { /* keep polling */ }
    }
    if (alive.current) setScanning(false);
  };

  const saveNet = async () => {
    const name = ssid.trim();
    if (!name) { setMsg('Pick or type a network name.'); return; }
    const joinedElsewhere = st?.sta.state === 'connected' && st.sta.ssid !== name;
    const ok = await act('Save network', () => api.save(auth, name, pass), joinedElsewhere
      ? `Saved ${name}. The device stays on ${st!.sta.ssid} for now, and can use ${name} if ${st!.sta.ssid} goes out of range.`
      : `Saved ${name}. The device will try to join it now, and your phone may drop off ${st?.ap.ssid || 'the device network'} for a moment.`);
    if (ok) { setSsid(''); setPass(''); }
  };

  const saveAp = async () => {
    const name = apSsid.trim() || st?.ap.ssid || '';
    if (apPass.length < 8) { setMsg('The device password needs at least 8 characters.'); return; }
    const ok = await act('Change device network', () => api.setAp(auth, name, apPass),
      `Changed. The device network restarts as ${name} in a moment; rejoin it with the new password.`);
    if (ok) { rememberAuth(apPass); setApPass(''); setApOpen(false); }
  };

  const sta = st?.sta;

  // Forgetting the network the device is on right now drops it off that network, and cuts off this
  // page too if it was reached through it. Ask for a second tap in that case.
  const forget = (name: string) => {
    const inUse = sta?.state === 'connected' && sta.ssid === name;
    if (inUse && confirmForget !== name) {
      setConfirmForget(name);
      setMsg(`The device is on ${name} right now. Forgetting it disconnects the device from it, and this page too if you reached it that way. Tap Confirm to forget it.`);
      return;
    }
    setConfirmForget(null);
    act('Forget network', () => api.forget(auth, name), `Forgot ${name}.`);
  };

  return (
    <View style={{ gap: 14 }}>
      {!!err && <Note warn>{err}</Note>}
      {!st && !err && <ActivityIndicator color={C.text} />}

      <View style={{ gap: 6 }}>
        <Text style={s.tag}>DEVICE PASSWORD</Text>
        <Note>The password of the device's own network. Needed to change anything here, and remembered on this phone.</Note>
        <TextInput style={s.input} value={auth} onChangeText={rememberAuth} secureTextEntry
                   placeholder="device network password" placeholderTextColor={C.muted}
                   autoCapitalize="none" autoCorrect={false} />
      </View>

      {st && (
        <View style={{ gap: 6 }}>
          <Text style={s.tag}>DEVICE NETWORK</Text>
          <Text style={s.text}>{st.ap.ssid}   <Text style={s.muted}>· {st.ap.ip} · {st.ap.clients} connected</Text></Text>
          {st.ap.publicDefaultPass && (
            <Note warn>This network still uses the public default password from the source code. Anyone nearby could join and control the device. Change it below.</Note>
          )}
          {!apOpen ? (
            <Row><Btn ghost label="Change name or password" onPress={() => { setApSsid(st.ap.ssid); setApOpen(true); }} /></Row>
          ) : (
            <View style={{ gap: 6 }}>
              <TextInput style={s.input} value={apSsid} onChangeText={setApSsid} placeholder="network name"
                         placeholderTextColor={C.muted} autoCapitalize="none" autoCorrect={false} />
              <TextInput style={s.input} value={apPass} onChangeText={setApPass} secureTextEntry
                         placeholder="new password (8+ characters)" placeholderTextColor={C.muted}
                         autoCapitalize="none" autoCorrect={false} />
              <Note>Every phone on the device network is disconnected and must rejoin with the new password.</Note>
              <Row>
                <Btn label="Save" onPress={saveAp} disabled={busy} />
                <Btn ghost label="Cancel" onPress={() => { setApOpen(false); setApPass(''); }} />
              </Row>
            </View>
          )}
        </View>
      )}

      {st && sta && (
        <View style={{ gap: 6 }}>
          <Text style={s.tag}>OTHER NETWORKS</Text>
          <Text style={s.text}>{staSummary(st)}{sta.state === 'connected' && sta.ip ? `   ` : ''}
            {sta.state === 'connected' && <Text style={s.muted}>{sta.ip} · {sta.rssi} dBm</Text>}
          </Text>
          {sta.held && (
            <Note>The device stops searching while a phone is on its network, so control never stutters. Tap Connect now to search anyway.</Note>
          )}
          {!!sta.lastError && sta.state !== 'connected' && <Note>Last attempt: {sta.lastError}</Note>}
          {st.saved.length > 0 && sta.state !== 'connected' && (
            <Row><Btn ghost label="Connect now" disabled={busy}
                      onPress={() => act('Connect', () => api.connect(auth), 'Searching now. Your phone may drop off the device network for a moment.')} /></Row>
          )}

          {st.saved.length === 0 && <Note>No saved networks. The device only runs its own network.</Note>}
          {st.saved.map(name => (
            <View key={name} style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: 8 }}>
              <Text style={[s.text, { flex: 1 }]} numberOfLines={1}>{name}</Text>
              <Btn ghost={confirmForget !== name} label={confirmForget === name ? 'Confirm' : 'Forget'} disabled={busy}
                   onPress={() => forget(name)} />
            </View>
          ))}
          <Note>{st.saved.length} of {st.maxSaved} saved.</Note>
        </View>
      )}

      {st && (
        <View style={{ gap: 6 }}>
          <Text style={s.tag}>ADD A NETWORK</Text>
          <Row>
            <Btn ghost label={scanning ? 'Scanning…' : 'Scan for networks'} onPress={doScan} disabled={scanning || busy} />
          </Row>
          <Note>Scanning briefly interrupts the connection to the device.</Note>
          {scan && scan.length === 0 && <Note>No networks found.</Note>}
          {scan && scan.map(n => (
            <Pressable key={n.ssid} onPress={() => { setSsid(n.ssid); setPass(''); }}
                       style={{ flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 6 }}>
              <Text style={[s.text, ssid === n.ssid && { fontWeight: '700' }]} numberOfLines={1}>
                {n.ssid}{n.saved ? '  ✓' : ''}
              </Text>
              <Text style={s.muted}>{n.secure ? '🔒 ' : ''}{bars(n.rssi)}</Text>
            </Pressable>
          ))}
          <TextInput style={s.input} value={ssid} onChangeText={setSsid} placeholder="network name"
                     placeholderTextColor={C.muted} autoCapitalize="none" autoCorrect={false} />
          <TextInput style={s.input} value={pass} onChangeText={setPass} secureTextEntry
                     placeholder="password (leave empty for an open network)" placeholderTextColor={C.muted}
                     autoCapitalize="none" autoCorrect={false} />
          <Row><Btn label="Save network" onPress={saveNet} disabled={busy} /></Row>
        </View>
      )}

      {!!msg && <Note>{msg}</Note>}
    </View>
  );
}
