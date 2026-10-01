// ConnectScreen.tsx — the "not connected" landing page.
//
// One job: get the app onto a T-DSP. Every way to connect is a card, and tapping the card IS the
// connect (no separate picker + button). While a card is connecting it shows a spinner and Cancel;
// if it fails, the reason appears under that card. A new tap always supersedes whatever attempt is
// in flight (App.tsx connectTo), so the page can never get stuck ignoring taps.
//
// Pure presentation: App.tsx builds the option list and owns the connection logic.
import React, { useState } from 'react';
import { View, Text, Pressable, TextInput, ScrollView, ActivityIndicator, StyleSheet } from 'react-native';
import { C } from './theme';
import WifiQr, { wifiQrPayload } from './WifiQr';

export type ConnKind = 'default' | 'wifi';

export interface ConnOption {
  id: string;          // 'wifi:<host>' | 'default'
  kind: ConnKind;
  host?: string;
  icon: string;
  title: string;
  subtitle: string;
  badge?: string;      // e.g. 'last used'
  help?: 'deviceNetwork';   // show the device network's name/password/QR under this card
  joinSsid?: string;        // tapping joins this access point first (Android app)
}

export interface ConnError { id: string; message: string }

// App-update controls for the disconnected state: Settings is only reachable once connected, but
// "is my app current?" matters most right here (and the phone is OFF the T-DSP network now, so the
// update server is reachable). App supplies the running bundle's facts and the actions.
export interface ConnAppUpdate {
  enabled: boolean;          // false on web / dev client
  bundle: string;            // short id of the running bundle, or 'built-in'
  channel: string;
  publishedAt: string;       // '' when unknown
  check: () => Promise<string>;   // resolves to a status line (restarts the app if it applied one)
  reload: () => void;             // plain bundle restart
}

export default function ConnectScreen({
  options, attemptId, attemptAuto, error, searching, onConnect, onCancel, footer, refreshControl,
  deviceNetwork, onOpenWifiSettings, onCopy, appUpdate,
}: {
  options: ConnOption[];
  attemptId: string | null;      // option currently connecting
  attemptAuto: boolean;          // that attempt was started automatically (boot / app resumed)
  error: ConnError | null;
  searching: boolean;            // mDNS browse running (native)
  onConnect: (o: ConnOption) => void;
  onCancel: () => void;
  footer?: string;
  refreshControl?: React.ReactElement<any>;   // pull-to-refresh (native)
  deviceNetwork?: { ssid: string; pass: string } | null;   // the T-DSP's own Wi-Fi, when the app knows it
  onOpenWifiSettings?: () => void;                         // system Wi-Fi picker (Android)
  onCopy?: (text: string) => unknown;                      // clipboard
  appUpdate?: ConnAppUpdate;                               // app version + OTA check (native builds)
}) {
  const [updMsg, setUpdMsg] = useState('');
  const [updBusy, setUpdBusy] = useState(false);
  const [manualOpen, setManualOpen] = useState(false);
  const [manual, setManual] = useState('');
  const [showNet, setShowNet] = useState(false);
  const [showQr, setShowQr] = useState(false);
  const [copied, setCopied] = useState(false);
  const copy = (text: string) => {
    Promise.resolve(onCopy?.(text)).then(() => { setCopied(true); setTimeout(() => setCopied(false), 2000); }).catch(() => {});
  };
  const manualId = manual.trim() ? 'wifi:' + manual.trim() : '';
  const manualBusy = !!manualId && attemptId === manualId && !options.some(o => o.id === manualId);

  const submitManual = () => {
    const host = manual.trim();
    if (!host) return;
    onConnect({ id: 'wifi:' + host, kind: 'wifi', host, icon: '⌨', title: host, subtitle: 'Wi-Fi address' });
  };

  const renderError = (id: string) =>
    error && error.id === id ? <Text style={st.error}>{error.message}</Text> : null;

  return (
    <ScrollView style={st.scroll} contentContainerStyle={st.content} keyboardShouldPersistTaps="handled" refreshControl={refreshControl}>
      <View style={st.col}>
        <View style={st.brandRow}>
          <View style={st.dot} />
          <Text style={st.brand}>T-DSP</Text>
          <Text style={st.brandStatus}>{attemptId ? (attemptAuto ? 'Reconnecting…' : 'Connecting…') : 'Not connected'}</Text>
        </View>

        <Text style={st.h1}>Connect to your T-DSP</Text>
        <Text style={st.lead}>Tap a way to connect.</Text>

        {options.map(o => {
          const busy = attemptId === o.id;
          return (
            <View key={o.id}>
              <Pressable
                onPress={() => (busy ? onCancel() : onConnect(o))}
                style={({ pressed }) => [st.card, busy && st.cardBusy, pressed && st.cardPressed]}
                accessibilityRole="button"
                accessibilityLabel={busy ? `Cancel connecting to ${o.title}` : `Connect to ${o.title}`}
              >
                <Text style={st.icon}>{o.icon}</Text>
                <View style={st.cardText}>
                  <View style={st.titleRow}>
                    <Text style={st.title} numberOfLines={1}>{o.title}</Text>
                    {!!o.badge && <Text style={st.badge}>{o.badge}</Text>}
                  </View>
                  <Text style={st.subtitle} numberOfLines={2}>{o.subtitle}</Text>
                </View>
                {busy ? (
                  <View style={st.right}>
                    <ActivityIndicator color={C.accent} size="small" />
                    <Text style={st.cancel}>Cancel</Text>
                  </View>
                ) : (
                  <Text style={st.chev}>❯</Text>
                )}
              </Pressable>
              {renderError(o.id)}
              {o.help === 'deviceNetwork' && deviceNetwork && (!showNet ? (
                <Pressable onPress={() => setShowNet(true)} style={({ pressed }) => [st.card, st.cardQuiet, pressed && st.cardPressed]} accessibilityRole="button">
                  <Text style={st.icon}>🔑</Text>
                  <View style={st.cardText}>
                    <Text style={st.title}>Show the Wi-Fi password</Text>
                    <Text style={st.subtitle} numberOfLines={2}>The T-DSP network name and password, copy, QR for another phone</Text>
                  </View>
                  <Text style={st.chev}>❯</Text>
                </Pressable>
              ) : (
                <View style={st.netPanel}>
                  <View style={st.kv}><Text style={st.k}>Network</Text><Text selectable style={st.v}>{o.joinSsid || deviceNetwork.ssid}</Text></View>
                  <View style={st.kvRow}>
                    <Text style={st.k}>Password</Text>
                    <Text selectable style={[st.v, st.pw]}>{deviceNetwork.pass}</Text>
                    {onCopy && (
                      <Pressable onPress={() => copy(deviceNetwork.pass)} style={({ pressed }) => [st.copyBtn, pressed && st.cardPressed]} accessibilityRole="button">
                        <Text style={st.copyText}>{copied ? 'Copied ✓' : 'Copy'}</Text>
                      </Pressable>
                    )}
                  </View>
                  {o.joinSsid ? (
                    <Pressable onPress={() => onConnect(o)} style={({ pressed }) => [st.go, st.goWide, pressed && st.cardPressed]} accessibilityRole="button">
                      <Text style={st.goText}>Connect to {o.joinSsid} Wi-Fi</Text>
                    </Pressable>
                  ) : (
                    <>
                      <Text style={st.hint}>Join this Wi-Fi in your phone's settings, then come back and tap T-DSP Access Point.</Text>
                      {onOpenWifiSettings && (
                        <Pressable onPress={onOpenWifiSettings} style={({ pressed }) => [st.go, st.goWide, pressed && st.cardPressed]} accessibilityRole="button">
                          <Text style={st.goText}>Open Wi-Fi settings</Text>
                        </Pressable>
                      )}
                    </>
                  )}
                  {!showQr ? (
                    <Pressable onPress={() => setShowQr(true)} style={st.linkRow}><Text style={st.link}>Share with another phone</Text></Pressable>
                  ) : (
                    <>
                      <WifiQr value={wifiQrPayload(o.joinSsid || deviceNetwork.ssid, deviceNetwork.pass)} size={200} />
                      <Text style={[st.hint, { textAlign: 'center' }]}>The other phone scans this with its camera to join.</Text>
                    </>
                  )}
                  <Pressable onPress={() => { setShowNet(false); setShowQr(false); }} style={st.linkRow}><Text style={st.link}>Hide</Text></Pressable>
                </View>
              ))}
            </View>
          );
        })}

        {!manualOpen ? (
          <Pressable onPress={() => setManualOpen(true)} style={({ pressed }) => [st.card, st.cardQuiet, pressed && st.cardPressed]}>
            <Text style={st.icon}>⌨</Text>
            <View style={st.cardText}>
              <Text style={st.title}>Another address</Text>
              <Text style={st.subtitle}>Type an IP or host name</Text>
            </View>
            <Text style={st.chev}>❯</Text>
          </Pressable>
        ) : (
          <View style={[st.card, st.manual]}>
            <TextInput
              style={st.input} value={manual} onChangeText={setManual} onSubmitEditing={submitManual}
              placeholder="192.168.1.50 or tdsp.local" placeholderTextColor={C.muted}
              autoCapitalize="none" autoCorrect={false} keyboardType="url" returnKeyType="go" autoFocus
            />
            <Pressable onPress={() => (manualBusy ? onCancel() : submitManual())}
                       style={({ pressed }) => [st.go, !manual.trim() && st.goOff, pressed && st.cardPressed]}
                       disabled={!manual.trim()}>
              {manualBusy ? <ActivityIndicator color={C.text} size="small" /> : <Text style={st.goText}>Connect</Text>}
            </Pressable>
          </View>
        )}
        {manualId && !options.some(o => o.id === manualId) ? renderError(manualId) : null}

        {searching && (
          <View style={[st.card, st.cardQuiet]} accessibilityRole="progressbar">
            <View style={st.iconBox}><ActivityIndicator color={C.muted} size="small" /></View>
            <View style={st.cardText}>
              <Text style={st.title}>Looking for T-DSP devices on this Wi-Fi…</Text>
              <Text style={st.subtitle} numberOfLines={2}>Each T-DSP found on this network appears above as its own card. Nothing yet.</Text>
            </View>
          </View>
        )}
        {!!footer && <Text style={[st.hint, st.footer]}>{footer}</Text>}

        {appUpdate && (
          <View style={st.updBox}>
            <Text style={st.updTitle}>App version &amp; updates</Text>
            <Text style={st.hint}>
              Bundle {appUpdate.bundle}{appUpdate.channel ? ' \u00b7 ' + appUpdate.channel : ''}{appUpdate.publishedAt ? ' \u00b7 published ' + appUpdate.publishedAt : ''}
            </Text>
            {appUpdate.enabled ? (
              <View style={st.updRow}>
                <Pressable style={[st.updBtn, updBusy && { opacity: 0.6 }]} disabled={updBusy}
                  onPress={async () => { setUpdBusy(true); setUpdMsg('Checking\u2026'); try { setUpdMsg(await appUpdate.check()); } finally { setUpdBusy(false); } }}
                  accessibilityRole="button">
                  <Text style={st.updBtnText}>Check for app update</Text>
                </Pressable>
                <Pressable style={[st.updBtn, st.updBtnGhost]} onPress={appUpdate.reload} accessibilityRole="button">
                  <Text style={st.updBtnText}>Reload app</Text>
                </Pressable>
              </View>
            ) : <Text style={st.hint}>Over-the-air updates are not available in this build.</Text>}
            {!!updMsg && <Text style={st.hint}>{updMsg}</Text>}
          </View>
        )}
      </View>
    </ScrollView>
  );
}

const st = StyleSheet.create({
  scroll: { flex: 1, backgroundColor: C.bg },
  content: { flexGrow: 1, alignItems: 'center', paddingHorizontal: 16, paddingTop: 20, paddingBottom: 40 },
  col: { width: '100%', maxWidth: 460 },
  brandRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 28 },
  dot: { width: 10, height: 10, borderRadius: 5, backgroundColor: '#f85149' },
  brand: { color: C.text, fontSize: 18, fontWeight: '800', letterSpacing: 0.5 },
  brandStatus: { color: C.muted, fontSize: 13, marginLeft: 'auto' },
  h1: { color: C.text, fontSize: 24, fontWeight: '700' },
  lead: { color: C.muted, fontSize: 14, marginTop: 4, marginBottom: 18 },
  card: {
    flexDirection: 'row', alignItems: 'center', gap: 14, minHeight: 68,
    backgroundColor: C.card, borderWidth: 1, borderColor: C.border, borderRadius: 12,
    paddingVertical: 12, paddingHorizontal: 14, marginBottom: 10,
  },
  cardQuiet: { backgroundColor: 'transparent' },
  cardBusy: { borderColor: C.accent },
  cardPressed: { opacity: 0.7 },
  icon: { fontSize: 22, width: 30, textAlign: 'center', color: C.text },
  cardText: { flex: 1, minWidth: 0 },
  titleRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  title: { color: C.text, fontSize: 16, fontWeight: '600', flexShrink: 1 },
  badge: { color: C.accent, fontSize: 11, fontWeight: '600', borderWidth: 1, borderColor: C.accent, borderRadius: 8, paddingHorizontal: 6, paddingVertical: 1, overflow: 'hidden' },
  subtitle: { color: C.muted, fontSize: 13, marginTop: 2 },
  chev: { color: C.muted, fontSize: 16 },
  right: { alignItems: 'center', gap: 2 },
  cancel: { color: C.muted, fontSize: 11 },
  error: { color: '#e3b341', fontSize: 13, marginTop: -4, marginBottom: 12, marginHorizontal: 6 },
  manual: { gap: 10 },
  input: { flex: 1, color: C.text, fontSize: 16, backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 8, paddingHorizontal: 10, paddingVertical: 10, minWidth: 0 },
  go: { backgroundColor: '#238636', borderRadius: 8, paddingHorizontal: 16, paddingVertical: 11, minWidth: 92, alignItems: 'center' },
  goOff: { opacity: 0.5 },
  goText: { color: C.text, fontSize: 15, fontWeight: '700' },
  searchRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 6 },
  hint: { color: C.muted, fontSize: 13 },
  footer: { marginTop: 16, lineHeight: 19 },
  iconBox: { width: 30, alignItems: 'center', justifyContent: 'center' },
  updBox: { marginTop: 28, paddingTop: 14, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.border, gap: 6 },
  updTitle: { color: C.text, fontSize: 14, fontWeight: '700' },
  updRow: { flexDirection: 'row', gap: 10, marginTop: 6, flexWrap: 'wrap' },
  updBtn: { backgroundColor: C.accent, paddingVertical: 10, paddingHorizontal: 14, borderRadius: 10 },
  updBtnGhost: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  updBtnText: { color: C.text, fontSize: 13, fontWeight: '600' },
  linkRow: { alignSelf: 'flex-start', paddingVertical: 6, paddingHorizontal: 6, marginTop: -4, marginBottom: 8 },
  link: { color: '#58a6ff', fontSize: 14, fontWeight: '600' },
  netPanel: { gap: 10, backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 12, padding: 14, marginTop: -2, marginBottom: 12 },
  kv: { flexDirection: 'row', alignItems: 'baseline', gap: 10 },
  k: { color: C.muted, fontSize: 13, width: 72 },
  v: { color: C.text, fontSize: 16, fontWeight: '600', flexShrink: 1 },
  pw: { fontSize: 20, letterSpacing: 0.5 },
  kvRow: { flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', gap: 10 },
  copyBtn: { borderWidth: 1, borderColor: C.border, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 6, marginLeft: 'auto' },
  copyText: { color: C.text, fontSize: 14, fontWeight: '600' },
  goWide: { alignSelf: 'stretch' },
});
