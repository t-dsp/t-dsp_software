// T-DSP Control — BLE control app for the ESP32 A2DP receiver.
// Home screen = status + master volume + one Connect/Disconnect toggle.
// Hamburger (☰) opens Settings: paired-source switcher, pairing, disconnect app.
// Needs a custom dev/preview build (BLE is native): see README.md.

import Slider from '@react-native-community/slider';
import { StatusBar } from 'expo-status-bar';
import { useState } from 'react';
import {
  ActivityIndicator,
  Modal,
  Pressable,
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { CMD, ConnState, TdspSource, useTdsp } from './src/tdspBle';

export default function App() {
  const {
    state,
    status,
    error,
    btReady,
    volume,
    sources,
    songs,
    instruments,
    scanAndConnect,
    disconnect,
    sendCommand,
    setVolume,
    connectSource,
    forgetSource,
    readSources,
    playSong,
    stopSong,
    setDxVoice,
    refreshCatalog,
  } = useTdsp();
  const [showSettings, setShowSettings] = useState(false);
  // Dexed instrument + selected song — tracked locally (no firmware readback yet).
  const [dxVoice, setDxVoiceState] = useState(0);
  const [song, setSong] = useState(0);
  const connected = state === 'connected';

  const openSettings = () => {
    readSources(); // refresh the paired list when the menu opens
    setShowSettings(true);
  };

  return (
    <SafeAreaView style={styles.screen}>
      <StatusBar style="light" />

      <View style={styles.header}>
        <View style={styles.headerText}>
          <Text style={styles.title}>T-DSP Control</Text>
          <Text style={styles.subtitle}>ESP32 Bluetooth receiver</Text>
        </View>
        {connected && (
          <Pressable onPress={openSettings} hitSlop={14} style={styles.iconBtn}>
            <Text style={styles.icon}>☰</Text>
          </Pressable>
        )}
      </View>

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
          <VolumeControl value={volume} onChange={setVolume} />
          <PrimaryButton
            label={status?.conn ? 'Disconnect Bluetooth Audio' : 'Connect Bluetooth Audio'}
            onPress={() => sendCommand(status?.conn ? CMD.DISCONNECT : CMD.RECONNECT)}
          />
        </>
      )}

      {!btReady && <Text style={styles.warn}>Turn on Bluetooth to continue.</Text>}
      {error ? <Text style={styles.error}>⚠ {error}</Text> : null}

      <SettingsModal
        visible={showSettings}
        onClose={() => setShowSettings(false)}
        sources={sources}
        discoverable={!!status?.disc}
        onConnectSource={connectSource}
        onForgetSource={forgetSource}
        onCommand={sendCommand}
        audioConnected={!!status?.conn}
        onDisconnectApp={() => {
          setShowSettings(false);
          disconnect();
        }}
        songs={songs}
        instruments={instruments}
        onPlaySong={playSong}
        onStopSong={stopSong}
        dxVoice={dxVoice}
        onSelectVoice={(i) => {
          setDxVoiceState(i);
          setDxVoice(i);
        }}
        song={song}
        onSelectSong={setSong}
        onRefreshCatalog={refreshCatalog}
      />
    </SafeAreaView>
  );
}

type SettingsPane = 'menu' | 'bluetooth' | 'midi';

function SettingsModal({
  visible,
  onClose,
  sources,
  discoverable,
  onConnectSource,
  onForgetSource,
  onCommand,
  audioConnected,
  onDisconnectApp,
  songs,
  instruments,
  onPlaySong,
  onStopSong,
  dxVoice,
  onSelectVoice,
  song,
  onSelectSong,
  onRefreshCatalog,
}: {
  visible: boolean;
  onClose: () => void;
  sources: TdspSource[];
  discoverable: boolean;
  onConnectSource: (addr: string) => void;
  onForgetSource: (addr: string) => void;
  onCommand: (op: number) => void;
  audioConnected: boolean;
  onDisconnectApp: () => void;
  songs: string[];
  instruments: string[];
  onPlaySong: (index: number) => void;
  onStopSong: () => void;
  dxVoice: number;
  onSelectVoice: (index: number) => void;
  song: number;
  onSelectSong: (index: number) => void;
  onRefreshCatalog: () => void;
}) {
  const [pane, setPane] = useState<SettingsPane>('menu');

  // Always reopen on the top-level menu.
  const close = () => {
    setPane('menu');
    onClose();
  };
  const title = pane === 'bluetooth' ? 'Bluetooth' : pane === 'midi' ? 'MIDI' : 'Settings';

  return (
    <Modal visible={visible} animationType="slide" onRequestClose={close}>
      <SafeAreaView style={styles.screen}>
        <View style={styles.header}>
          {pane === 'menu' ? (
            <Text style={styles.title}>{title}</Text>
          ) : (
            <Pressable onPress={() => setPane('menu')} hitSlop={14} style={styles.backBtn}>
              <Text style={styles.icon}>‹</Text>
              <Text style={styles.backTitle}>{title}</Text>
            </Pressable>
          )}
          <Pressable onPress={close} hitSlop={14} style={styles.iconBtn}>
            <Text style={styles.icon}>✕</Text>
          </Pressable>
        </View>

        <ScrollView contentContainerStyle={styles.settingsBody}>
          {pane === 'menu' && (
            <>
              <MenuRow label="Bluetooth" detail="Pairing & paired sources" onPress={() => setPane('bluetooth')} />
              <MenuRow label="MIDI" detail="Dexed synth" onPress={() => setPane('midi')} />
              <Text style={styles.sectionLabel}>App</Text>
              <SecondaryButton label="Disconnect App" onPress={onDisconnectApp} />
            </>
          )}

          {pane === 'bluetooth' && (
            <>
              <Text style={styles.sectionLabel}>Bluetooth Audio</Text>
              <Text style={styles.dim}>
                {audioConnected ? 'A source is connected and streaming.' : 'No source connected.'}
              </Text>
              <View style={{ height: 10 }} />
              <SecondaryButton
                label={audioConnected ? 'Disconnect Bluetooth Audio' : 'Reconnect Last Source'}
                onPress={() => onCommand(audioConnected ? CMD.DISCONNECT : CMD.RECONNECT)}
              />

              <Text style={styles.sectionLabel}>Paired Sources</Text>
              {sources.length === 0 ? (
                <Text style={styles.dim}>
                  No paired phones yet. Use “Enter Pairing Mode” below, then pair T-DSP from your phone’s
                  Bluetooth settings.
                </Text>
              ) : (
                sources.map((s) => (
                  <SourceRow
                    key={s.a}
                    source={s}
                    onConnect={() => onConnectSource(s.a)}
                    onForget={() => onForgetSource(s.a)}
                  />
                ))
              )}

              <Text style={styles.sectionLabel}>Pairing</Text>
              {discoverable && <Text style={styles.pairingHint}>● In pairing mode — discoverable as “T-DSP”</Text>}
              <SecondaryButton
                label={discoverable ? 'End Pairing Mode' : 'Enter Pairing Mode'}
                onPress={() => onCommand(discoverable ? CMD.END_PAIRING : CMD.PAIRING_MODE)}
              />
            </>
          )}

          {pane === 'midi' && (
            <>
              <Text style={styles.sectionLabel}>Dexed</Text>
              <Text style={styles.dim}>6-op FM synth, played by the MIDI IN port and the songs below.</Text>

              <Text style={styles.sectionLabel}>Song</Text>
              <Dropdown label="Song" options={songs} value={song} onSelect={onSelectSong} />
              <View style={{ height: 8 }} />
              <PrimaryButton label={`▶  Play ${songs[song] ?? 'Song'}`} onPress={() => onPlaySong(song)} />
              <SecondaryButton label="Stop" onPress={onStopSong} />
              <SecondaryButton label="↻  Refresh Songs (after adding via USB)" onPress={onRefreshCatalog} />

              <Text style={styles.sectionLabel}>Instrument</Text>
              <Dropdown label="Instrument" options={instruments} value={dxVoice} onSelect={onSelectVoice} />
            </>
          )}
        </ScrollView>
      </SafeAreaView>
    </Modal>
  );
}

function MenuRow({ label, detail, onPress }: { label: string; detail: string; onPress: () => void }) {
  return (
    <Pressable style={({ pressed }) => [styles.menuRow, pressed && styles.btnPressed]} onPress={onPress}>
      <View style={{ flex: 1 }}>
        <Text style={styles.menuLabel}>{label}</Text>
        <Text style={styles.menuDetail}>{detail}</Text>
      </View>
      <Text style={styles.menuChevron}>›</Text>
    </Pressable>
  );
}

// Collapsible dropdown: a header row showing the current value that expands the
// option list on tap and collapses again on select (pure JS, no native picker).
function Dropdown({
  label,
  options,
  value,
  onSelect,
}: {
  label: string;
  options: string[];
  value: number;
  onSelect: (index: number) => void;
}) {
  const [open, setOpen] = useState(false);
  return (
    <View>
      <Pressable style={({ pressed }) => [styles.menuRow, pressed && styles.btnPressed]} onPress={() => setOpen((o) => !o)}>
        <View style={{ flex: 1 }}>
          <Text style={styles.menuDetail}>{label}</Text>
          <Text style={styles.menuLabel}>{options[value] ?? '—'}</Text>
        </View>
        <Text style={styles.menuChevron}>{open ? '▾' : '▸'}</Text>
      </Pressable>
      {open &&
        options.map((opt, i) => (
          <InstrumentRow
            key={opt}
            name={opt}
            selected={i === value}
            onPress={() => {
              onSelect(i);
              setOpen(false);
            }}
          />
        ))}
    </View>
  );
}

function InstrumentRow({ name, selected, onPress }: { name: string; selected: boolean; onPress: () => void }) {
  return (
    <Pressable style={({ pressed }) => [styles.instRow, selected && styles.instRowOn, pressed && styles.btnPressed]} onPress={onPress}>
      <Text style={[styles.instName, selected && styles.instNameOn]}>{name}</Text>
      {selected && <Text style={styles.instCheck}>✓</Text>}
    </Pressable>
  );
}

function SourceRow({
  source,
  onConnect,
  onForget,
}: {
  source: TdspSource;
  onConnect: () => void;
  onForget: () => void;
}) {
  return (
    <View style={styles.srcRow}>
      <Pressable style={styles.srcMain} onPress={source.c ? undefined : onConnect} disabled={source.c}>
        <Text style={styles.srcName} numberOfLines={1}>
          {source.n}
        </Text>
        <Text style={[styles.srcState, source.c && styles.srcStateOn]}>
          {source.c ? '● connected' : 'tap to connect'}
        </Text>
      </Pressable>
      <Pressable style={styles.srcForget} onPress={onForget} hitSlop={8}>
        <Text style={styles.srcForgetTxt}>Forget</Text>
      </Pressable>
    </View>
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
          <Row label="Audio source" value={status?.conn ? status.peer || 'connected' : 'none'} />
          <Row label="Pairing mode" value={status?.disc ? 'ON — discoverable' : 'off'} highlight={status?.disc} />
        </>
      )}
    </View>
  );
}

function VolumeControl({ value, onChange }: { value: number; onChange: (v: number) => void }) {
  return (
    <View style={styles.volCard}>
      <View style={styles.volHeader}>
        <Text style={styles.volLabel}>Headphone Volume</Text>
        <Text style={styles.volValue}>{Math.round(value)}%</Text>
      </View>
      <Slider
        style={styles.slider}
        minimumValue={0}
        maximumValue={100}
        step={1}
        value={value}
        onValueChange={onChange}
        minimumTrackTintColor="#238636"
        maximumTrackTintColor="#30363d"
        thumbTintColor="#3fb950"
      />
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
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'flex-start', marginBottom: 24 },
  headerText: { flex: 1 },
  title: { color: '#fff', fontSize: 32, fontWeight: '700' },
  subtitle: { color: '#8b949e', fontSize: 15, marginTop: 2 },
  iconBtn: { padding: 6 },
  icon: { color: '#e6edf3', fontSize: 26, fontWeight: '700' },
  card: { backgroundColor: '#161b22', borderRadius: 14, padding: 18, marginBottom: 28, borderWidth: 1, borderColor: '#21262d' },
  volCard: { backgroundColor: '#161b22', borderRadius: 14, paddingHorizontal: 18, paddingVertical: 14, marginBottom: 16, borderWidth: 1, borderColor: '#21262d' },
  volHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 4 },
  volLabel: { color: '#e6edf3', fontSize: 16, fontWeight: '600' },
  volValue: { color: '#3fb950', fontSize: 16, fontWeight: '700' },
  slider: { width: '100%', height: 40 },
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
  settingsBody: { paddingBottom: 40 },
  backBtn: { flexDirection: 'row', alignItems: 'center', flex: 1 },
  backTitle: { color: '#fff', fontSize: 32, fontWeight: '700', marginLeft: 4 },
  menuRow: { flexDirection: 'row', alignItems: 'center', backgroundColor: '#161b22', borderRadius: 12, borderWidth: 1, borderColor: '#21262d', paddingVertical: 16, paddingHorizontal: 16, marginBottom: 12 },
  menuLabel: { color: '#e6edf3', fontSize: 17, fontWeight: '600' },
  menuDetail: { color: '#8b949e', fontSize: 13, marginTop: 2 },
  menuChevron: { color: '#6e7681', fontSize: 24, fontWeight: '700' },
  instRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', backgroundColor: '#161b22', borderRadius: 12, borderWidth: 1, borderColor: '#21262d', paddingVertical: 14, paddingHorizontal: 16, marginBottom: 10 },
  instRowOn: { borderColor: '#238636', backgroundColor: '#12261a' },
  instName: { color: '#e6edf3', fontSize: 16, fontWeight: '600' },
  instNameOn: { color: '#3fb950' },
  instCheck: { color: '#3fb950', fontSize: 18, fontWeight: '700' },
  sectionLabel: { color: '#8b949e', fontSize: 13, fontWeight: '700', textTransform: 'uppercase', letterSpacing: 1, marginTop: 18, marginBottom: 10 },
  dim: { color: '#6e7681', fontSize: 14, lineHeight: 20 },
  pairingHint: { color: '#3fb950', fontSize: 13, fontWeight: '600', marginBottom: 10 },
  srcRow: { flexDirection: 'row', alignItems: 'center', backgroundColor: '#161b22', borderRadius: 12, borderWidth: 1, borderColor: '#21262d', marginBottom: 10 },
  srcMain: { flex: 1, paddingVertical: 14, paddingHorizontal: 16 },
  srcName: { color: '#e6edf3', fontSize: 16, fontWeight: '600' },
  srcState: { color: '#8b949e', fontSize: 13, marginTop: 2 },
  srcStateOn: { color: '#3fb950', fontWeight: '700' },
  srcForget: { paddingVertical: 14, paddingHorizontal: 16 },
  srcForgetTxt: { color: '#f85149', fontSize: 14, fontWeight: '600' },
});
