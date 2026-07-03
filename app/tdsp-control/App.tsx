// T-DSP Control — minimal BLE control screen for the ESP32 A2DP receiver.
// Scan/connect to the "T-DSP" device, put it in pairing mode, see live status.
// Needs a custom dev/preview build (BLE is native): see README.md.

import { StatusBar } from 'expo-status-bar';
import {
  ActivityIndicator,
  Pressable,
  SafeAreaView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { CMD, ConnState, useTdsp } from './src/tdspBle';

export default function App() {
  const { state, status, error, btReady, scanAndConnect, disconnect, sendCommand } = useTdsp();
  const connected = state === 'connected';

  return (
    <SafeAreaView style={styles.screen}>
      <StatusBar style="light" />
      <Text style={styles.title}>T-DSP Control</Text>
      <Text style={styles.subtitle}>ESP32 Bluetooth receiver</Text>

      <ConnCard state={state} btReady={btReady} status={status} />

      {!connected ? (
        <PrimaryButton
          label={state === 'scanning' ? 'Scanning…' : state === 'connecting' ? 'Connecting…' : 'Connect to T-DSP'}
          busy={state === 'scanning' || state === 'connecting'}
          disabled={!btReady || state !== 'idle'}
          onPress={scanAndConnect}
        />
      ) : (
        <>
          <PrimaryButton label="Enter Pairing Mode" onPress={() => sendCommand(CMD.PAIRING_MODE)} />
          <SecondaryButton label="End Pairing Mode" onPress={() => sendCommand(CMD.END_PAIRING)} />
          <SecondaryButton label="Disconnect Audio Source" onPress={() => sendCommand(CMD.DISCONNECT)} />
          <SecondaryButton label="Forget Paired Device" onPress={() => sendCommand(CMD.FORGET)} />
          <SecondaryButton label="Disconnect App" onPress={disconnect} />
        </>
      )}

      {!btReady && <Text style={styles.warn}>Turn on Bluetooth to continue.</Text>}
      {error ? <Text style={styles.error}>⚠ {error}</Text> : null}
    </SafeAreaView>
  );
}

function ConnCard({
  state,
  btReady,
  status,
}: {
  state: ConnState;
  btReady: boolean;
  status: ReturnType<typeof useTdsp>['status'];
}) {
  const line =
    state === 'connected'
      ? 'Connected'
      : state === 'connecting'
        ? 'Connecting…'
        : state === 'scanning'
          ? 'Scanning…'
          : btReady
            ? 'Not connected'
            : 'Bluetooth off';
  return (
    <View style={styles.card}>
      <Row label="Link" value={line} />
      {state === 'connected' && (
        <>
          <Row label="Audio source" value={status?.conn ? (status.peer || 'connected') : 'none'} />
          <Row label="Pairing mode" value={status?.disc ? 'ON — discoverable' : 'off'} highlight={status?.disc} />
        </>
      )}
    </View>
  );
}

function Row({ label, value, highlight }: { label: string; value: string; highlight?: boolean }) {
  return (
    <View style={styles.row}>
      <Text style={styles.rowLabel}>{label}</Text>
      <Text style={[styles.rowValue, highlight ? styles.rowValueHot : null]}>{value}</Text>
    </View>
  );
}

function PrimaryButton({
  label,
  onPress,
  busy,
  disabled,
}: {
  label: string;
  onPress: () => void;
  busy?: boolean;
  disabled?: boolean;
}) {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      style={({ pressed }) => [styles.btn, styles.btnPrimary, disabled ? styles.btnDisabled : null, pressed && styles.btnPressed]}
    >
      {busy ? <ActivityIndicator color="#fff" /> : <Text style={styles.btnPrimaryText}>{label}</Text>}
    </Pressable>
  );
}

function SecondaryButton({ label, onPress }: { label: string; onPress: () => void }) {
  return (
    <Pressable onPress={onPress} style={({ pressed }) => [styles.btn, styles.btnSecondary, pressed && styles.btnPressed]}>
      <Text style={styles.btnSecondaryText}>{label}</Text>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: '#0d1117', paddingHorizontal: 24, paddingTop: 60 },
  title: { color: '#fff', fontSize: 32, fontWeight: '700' },
  subtitle: { color: '#8b949e', fontSize: 15, marginTop: 2, marginBottom: 24 },
  card: { backgroundColor: '#161b22', borderRadius: 14, padding: 18, marginBottom: 28, borderWidth: 1, borderColor: '#21262d' },
  row: { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 6 },
  rowLabel: { color: '#8b949e', fontSize: 15 },
  rowValue: { color: '#e6edf3', fontSize: 15, fontWeight: '600' },
  rowValueHot: { color: '#3fb950' },
  btn: { borderRadius: 12, paddingVertical: 16, alignItems: 'center', marginBottom: 12 },
  btnPrimary: { backgroundColor: '#238636' },
  btnPrimaryText: { color: '#fff', fontSize: 17, fontWeight: '700' },
  btnSecondary: { backgroundColor: '#21262d' },
  btnSecondaryText: { color: '#e6edf3', fontSize: 16, fontWeight: '600' },
  btnDisabled: { opacity: 0.4 },
  btnPressed: { opacity: 0.7 },
  warn: { color: '#d29922', fontSize: 14, marginTop: 8 },
  error: { color: '#f85149', fontSize: 14, marginTop: 12 },
});
