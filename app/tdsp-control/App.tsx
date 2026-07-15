// T-DSP Control — BLE control app for the ESP32 A2DP receiver.
// Home screen = status + master volume + one Connect/Disconnect toggle.
// Hamburger (☰) opens Settings: paired-source switcher, pairing, disconnect app.
// Needs a custom dev/preview build (BLE is native): see README.md.

import Slider from '@react-native-community/slider';
import { StatusBar } from 'expo-status-bar';
import { useEffect, useRef, useState } from 'react';
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
import { CMD, ConnState, SynthInfo, TdspSource, useTdsp } from './src/tdspBle';

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
    drums,
    drumKits,
    isGM,
    drumsOk,
    synth,
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
    setHpf,
    setMidiMode,
    setReplayGain,
    setLoop,
    playDrum,
    stopDrum,
    setDrumKit,
    setDrumSpeed,
    setDrumVol,
    setBpm,
    setDrumSynchro,
    setPressure,
    setModWheel,
    setLfoMode,
    setTimbre,
    refreshCatalog,
  } = useTdsp();
  const [showSettings, setShowSettings] = useState(false);
  // Dexed instrument + selected song — tracked locally (no firmware readback yet).
  const [dxVoice, setDxVoiceState] = useState(0);
  const [song, setSong] = useState(0);
  // TAC5212 DAC highpass — tracked locally (no firmware readback yet). Default off;
  // hpfCutIdx is the last-picked cutoff (0=1Hz, 1=12Hz, 2=96Hz) → filter mode idx+1.
  const [hpfOn, setHpfOn] = useState(false);
  const [hpfCutIdx, setHpfCutIdx] = useState(1); // 12 Hz
  // MIDI vs MPE mode — mirrors status.mpe once per connection, then local toggle wins.
  const [mpe, setMpe] = useState(false);
  // ReplayGain loudness normalization — mirrors status.rg once per connection (device default on).
  const [rg, setRg] = useState(true);
  // Loop the current song (local UI state; sent to the device on change).
  const [loop, setLoopState] = useState(false);
  // Drums — local UI state (no firmware readback). Groove/kit indices + speed/level %.
  const [drumGroove, setDrumGroove] = useState(0);
  const [drumKit, setDrumKitState] = useState(0);
  const [drumSpeed, setDrumSpeedState] = useState(100);
  const [drumVol, setDrumVolState] = useState(100);
  const [bpm, setBpmState] = useState(120);          // master tempo (song + drum)
  const [drumSynchro, setDrumSynchroState] = useState(false);
  // Expression routing bitmasks (1=volume 2=brightness 4=vibrato 8=tremolo).
  const [pressMask, setPressMask] = useState(3);   // pressure: default vol+bright
  const [modMask, setModMask] = useState(4);       // mod wheel: default vibrato (no volume bit)
  const [timbreMask, setTimbreMask] = useState(3); // CC74 timbre (MPE Y): default volume + brightness (punchy)
  const [lfoForce, setLfoForce] = useState(true);  // force LFO so vib/trem work on any patch
  const connected = state === 'connected';

  // Initialize the HPF controls from the device's reported state once per
  // connection (status.hpf: 0=off, 1/2/3 = 1/12/96 Hz). Mirrors the volume-init
  // pattern in useTdsp; after this the local toggle is the source of truth.
  const hpfInitedRef = useRef(false);
  useEffect(() => {
    if (!connected) {
      hpfInitedRef.current = false;
      return;
    }
    if (status && !hpfInitedRef.current) {
      hpfInitedRef.current = true;
      const m = status.hpf ?? 0;
      setHpfOn(m !== 0);
      if (m > 0) setHpfCutIdx(m - 1);
      setMpe(!!status.mpe);
      setRg(status.rg !== false);
    }
  }, [connected, status]);

  const onToggleMpe = () => {
    const next = !mpe;
    setMpe(next);
    setMidiMode(next);
  };
  const onToggleRg = () => {
    const next = !rg;
    setRg(next);
    setReplayGain(next);
  };
  const onToggleLoop = () => {
    const next = !loop;
    setLoopState(next);
    setLoop(next);
  };
  const onTogglePressBit = (bit: number) => {
    const next = pressMask ^ bit;
    setPressMask(next);
    setPressure(next);
  };
  const onToggleModBit = (bit: number) => {
    const next = modMask ^ bit;
    setModMask(next);
    setModWheel(next);
  };
  const onToggleTimbreBit = (bit: number) => {
    const next = timbreMask ^ bit;
    setTimbreMask(next);
    setTimbre(next);
  };
  const onSetLfoForce = (force: boolean) => {
    setLfoForce(force);
    setLfoMode(force);
  };

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
          label={state === 'scanning' ? 'Scanning…' : state === 'connecting' ? 'Connecting…' : 'Connect App'}
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
          <SecondaryButton label="Disconnect App" onPress={disconnect} />
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
        songs={songs}
        instruments={instruments}
        synth={synth}
        onPlaySong={playSong}
        onStopSong={stopSong}
        loop={loop}
        onToggleLoop={onToggleLoop}
        drums={drums}
        drumKits={drumKits}
        drumGroove={drumGroove}
        onSelectDrum={setDrumGroove}
        onPlayDrum={(i) => {
          setDrumGroove(i);
          playDrum(i);
        }}
        onStopDrum={stopDrum}
        drumKit={drumKit}
        onSelectDrumKit={(i) => {
          setDrumKitState(i);
          setDrumKit(i);
        }}
        drumSpeed={drumSpeed}
        onPreviewDrumSpeed={setDrumSpeedState}
        onCommitDrumSpeed={setDrumSpeed}
        drumVol={drumVol}
        onPreviewDrumVol={setDrumVolState}
        onCommitDrumVol={setDrumVol}
        bpm={bpm}
        onPreviewBpm={setBpmState}
        onCommitBpm={setBpm}
        drumSynchro={drumSynchro}
        onToggleSynchro={() => {
          const next = !drumSynchro;
          setDrumSynchroState(next);
          setDrumSynchro(next);
        }}
        isGM={drumsOk}
        kitsOk={isGM}
        pressMask={pressMask}
        onTogglePressBit={onTogglePressBit}
        modMask={modMask}
        onToggleModBit={onToggleModBit}
        timbreMask={timbreMask}
        onToggleTimbreBit={onToggleTimbreBit}
        lfoForce={lfoForce}
        onSetLfoForce={onSetLfoForce}
        dxVoice={dxVoice}
        onSelectVoice={(i) => {
          setDxVoiceState(i);
          setDxVoice(i);
        }}
        song={song}
        onSelectSong={setSong}
        onRefreshCatalog={refreshCatalog}
        hpfOn={hpfOn}
        hpfCutIdx={hpfCutIdx}
        onToggleHpf={() => {
          const next = !hpfOn;
          setHpfOn(next);
          setHpf(next ? hpfCutIdx + 1 : 0);
        }}
        onSelectHpfCut={(i) => {
          setHpfCutIdx(i);
          if (hpfOn) setHpf(i + 1);
        }}
        mpe={mpe}
        onToggleMpe={onToggleMpe}
        rg={rg}
        onToggleRg={onToggleRg}
      />
    </SafeAreaView>
  );
}

type SettingsPane = 'menu' | 'bluetooth' | 'midi' | 'drums' | 'tac5212';

// TAC5212 DAC highpass cutoffs — dropdown index maps to filter mode (index + 1),
// i.e. 0→1 Hz (mode 1), 1→12 Hz (mode 2), 2→96 Hz (mode 3). Mode 0 = off.
const HPF_CUTOFFS = ['1 Hz', '12 Hz', '96 Hz'];

function SettingsModal({
  visible,
  onClose,
  sources,
  discoverable,
  onConnectSource,
  onForgetSource,
  onCommand,
  audioConnected,
  songs,
  instruments,
  synth,
  onPlaySong,
  onStopSong,
  loop,
  onToggleLoop,
  drums,
  drumKits,
  drumGroove,
  onSelectDrum,
  onPlayDrum,
  onStopDrum,
  drumKit,
  onSelectDrumKit,
  drumSpeed,
  onPreviewDrumSpeed,
  onCommitDrumSpeed,
  drumVol,
  onPreviewDrumVol,
  onCommitDrumVol,
  bpm,
  onPreviewBpm,
  onCommitBpm,
  drumSynchro,
  onToggleSynchro,
  isGM,
  kitsOk,
  pressMask,
  onTogglePressBit,
  modMask,
  onToggleModBit,
  timbreMask,
  onToggleTimbreBit,
  lfoForce,
  onSetLfoForce,
  dxVoice,
  onSelectVoice,
  song,
  onSelectSong,
  onRefreshCatalog,
  hpfOn,
  hpfCutIdx,
  onToggleHpf,
  onSelectHpfCut,
  mpe,
  onToggleMpe,
  rg,
  onToggleRg,
}: {
  visible: boolean;
  onClose: () => void;
  sources: TdspSource[];
  discoverable: boolean;
  onConnectSource: (addr: string) => void;
  onForgetSource: (addr: string) => void;
  onCommand: (op: number) => void;
  audioConnected: boolean;
  songs: string[];
  instruments: string[];
  synth: SynthInfo;
  onPlaySong: (index: number) => void;
  onStopSong: () => void;
  loop: boolean;
  onToggleLoop: () => void;
  drums: string[];
  drumKits: string[];
  drumGroove: number;
  onSelectDrum: (index: number) => void;
  onPlayDrum: (index: number) => void;
  onStopDrum: () => void;
  drumKit: number;
  onSelectDrumKit: (index: number) => void;
  drumSpeed: number;
  onPreviewDrumSpeed: (pct: number) => void;
  onCommitDrumSpeed: (pct: number) => void;
  drumVol: number;
  onPreviewDrumVol: (pct: number) => void;
  onCommitDrumVol: (pct: number) => void;
  bpm: number;
  onPreviewBpm: (bpm: number) => void;
  onCommitBpm: (bpm: number) => void;
  drumSynchro: boolean;
  onToggleSynchro: () => void;
  isGM: boolean;
  kitsOk: boolean;
  pressMask: number;
  onTogglePressBit: (bit: number) => void;
  modMask: number;
  onToggleModBit: (bit: number) => void;
  timbreMask: number;
  onToggleTimbreBit: (bit: number) => void;
  lfoForce: boolean;
  onSetLfoForce: (force: boolean) => void;
  dxVoice: number;
  onSelectVoice: (index: number) => void;
  song: number;
  onSelectSong: (index: number) => void;
  onRefreshCatalog: () => void;
  hpfOn: boolean;
  hpfCutIdx: number;
  onToggleHpf: () => void;
  onSelectHpfCut: (index: number) => void;
  mpe: boolean;
  onToggleMpe: () => void;
  rg: boolean;
  onToggleRg: () => void;
}) {
  const [pane, setPane] = useState<SettingsPane>('menu');

  // Always reopen on the top-level menu.
  const close = () => {
    setPane('menu');
    onClose();
  };
  const title =
    pane === 'bluetooth' ? 'Bluetooth'
      : pane === 'midi' ? 'MIDI'
        : pane === 'drums' ? 'Drums'
          : pane === 'tac5212' ? 'TAC5212'
            : 'Settings';

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
              <MenuRow label="MIDI" detail={`${synth.name} synth`} onPress={() => setPane('midi')} />
              <MenuRow
                label="Drums"
                detail={drums.length ? `${drums.length} grooves` : 'Add grooves via USB'}
                onPress={() => setPane('drums')}
              />
              <MenuRow label="TAC5212" detail="Codec highpass filter" onPress={() => setPane('tac5212')} />
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
              <Text style={styles.sectionLabel}>{synth.name}</Text>
              <Text style={styles.dim}>{synth.description}</Text>

              <Text style={styles.sectionLabel}>Input Mode</Text>
              <Text style={styles.dim}>
                MPE gives per-note pitch bend + pressure for expressive controllers
                (e.g. LinnStrument over USB or DIN). Normal MIDI keeps channel 10 as GM drums.
              </Text>
              <View style={{ height: 10 }} />
              <SecondaryButton
                label={mpe ? 'Mode: MPE (per-note expression)' : 'Mode: Normal MIDI'}
                onPress={onToggleMpe}
              />

              <Text style={styles.sectionLabel}>Loudness</Text>
              <Text style={styles.dim}>
                ReplayGain evens out perceived loudness across voices and GM programs
                (K-weighted). Off plays the synth's raw output.
              </Text>
              <View style={{ height: 10 }} />
              <SecondaryButton
                label={rg ? 'ReplayGain: On' : 'ReplayGain: Off'}
                onPress={onToggleRg}
              />

              <Text style={styles.sectionLabel}>Expression — controller → sound</Text>

              <Text style={styles.dim}>Mod Wheel (every keyboard)</Text>
              <SecondaryButton label={`${modMask & 4 ? '☑' : '☐'}  Mod Wheel → Vibrato`} onPress={() => onToggleModBit(4)} />
              <SecondaryButton label={`${modMask & 8 ? '☑' : '☐'}  Mod Wheel → Tremolo`} onPress={() => onToggleModBit(8)} />
              <SecondaryButton label={`${modMask & 2 ? '☑' : '☐'}  Mod Wheel → Brightness`} onPress={() => onToggleModBit(2)} />

              <View style={{ height: 10 }} />
              <Text style={styles.dim}>Timbre — CC74 slide (MPE Y-axis)</Text>
              <SecondaryButton label={`${timbreMask & 1 ? '☑' : '☐'}  Timbre → Volume`} onPress={() => onToggleTimbreBit(1)} />
              <SecondaryButton label={`${timbreMask & 2 ? '☑' : '☐'}  Timbre → Brightness`} onPress={() => onToggleTimbreBit(2)} />
              <SecondaryButton label={`${timbreMask & 4 ? '☑' : '☐'}  Timbre → Vibrato`} onPress={() => onToggleTimbreBit(4)} />
              <SecondaryButton label={`${timbreMask & 8 ? '☑' : '☐'}  Timbre → Tremolo`} onPress={() => onToggleTimbreBit(8)} />

              <View style={{ height: 10 }} />
              <Text style={styles.dim}>Pressure (aftertouch / MPE Z)</Text>
              <SecondaryButton label={`${pressMask & 1 ? '☑' : '☐'}  Pressure → Volume`} onPress={() => onTogglePressBit(1)} />
              <SecondaryButton label={`${pressMask & 2 ? '☑' : '☐'}  Pressure → Brightness`} onPress={() => onTogglePressBit(2)} />
              <SecondaryButton label={`${pressMask & 4 ? '☑' : '☐'}  Pressure → Vibrato`} onPress={() => onTogglePressBit(4)} />
              <SecondaryButton label={`${pressMask & 8 ? '☑' : '☐'}  Pressure → Tremolo`} onPress={() => onTogglePressBit(8)} />

              <View style={{ height: 10 }} />
              <Text style={styles.dim}>
                LFO for Vibrato/Tremolo. Force = works on any patch; Respect = only instruments tagged [V]/[T].
              </Text>
              <SecondaryButton
                label={lfoForce ? 'LFO: Force on any patch' : 'LFO: Respect patch [V]/[T]'}
                onPress={() => onSetLfoForce(!lfoForce)}
              />

              <Text style={styles.sectionLabel}>Song</Text>
              <Dropdown label="Song" options={songs} value={song} onSelect={onSelectSong} />
              <View style={{ height: 8 }} />
              <PrimaryButton label={`▶  Play ${songs[song] ?? 'Song'}`} onPress={() => onPlaySong(song)} />
              <SecondaryButton label="Stop" onPress={onStopSong} />
              <SecondaryButton label={loop ? '☑  Loop: On' : '☐  Loop: Off'} onPress={onToggleLoop} />
              <SecondaryButton label="↻  Refresh Songs (after adding via USB)" onPress={onRefreshCatalog} />

              <Text style={styles.sectionLabel}>Instrument</Text>
              <Dropdown label="Instrument" options={instruments} value={dxVoice} onSelect={onSelectVoice} />
              <Stepper
                count={instruments.length}
                value={dxVoice}
                onStep={(i) => onSelectVoice(i)}
              />
            </>
          )}

          {pane === 'drums' && (
            <>
              <Text style={styles.dim}>
                A looping drum groove plays under whatever you perform live on the keyboard, through the
                current engine. OPLL renders 5 rhythm sounds; SF2/TSF/OPL3 play every drum.
              </Text>
              {!isGM && (
                <Text style={styles.pairingHint}>
                  ⚠ {synth.name} has no channel-10 drum map — grooves stay silent. Flash an OPLL / OPL3 / SF2 / TSF build.
                </Text>
              )}

              <StepSlider
                label="Tempo"
                unit=" bpm"
                value={bpm}
                min={60}
                max={200}
                step={5}
                onPreview={onPreviewBpm}
                onCommit={onCommitBpm}
              />
              <Text style={styles.dim}>One tempo for the song AND the drums — they lock together and move as you drag.</Text>
              <SecondaryButton
                label={drumSynchro ? '☑  Synchro start — begin on your first note' : '☐  Synchro start — begin on Play'}
                onPress={onToggleSynchro}
              />

              <Text style={styles.sectionLabel}>Groove</Text>
              {drums.length === 0 ? (
                <Text style={styles.dim}>
                  No grooves on the SD card yet. Add them with tools/fetch_drums.py (they land in /drums),
                  then tap “Refresh” below.
                </Text>
              ) : (
                <>
                  <Dropdown label="Groove" options={drums} value={drumGroove} onSelect={onSelectDrum} />
                  <Stepper count={drums.length} value={drumGroove} onStep={onSelectDrum} />
                  <View style={{ height: 8 }} />
                  <PrimaryButton
                    label={`▶  Play ${drums[drumGroove] ?? 'Groove'}`}
                    onPress={() => onPlayDrum(drumGroove)}
                  />
                  <SecondaryButton label="Stop" onPress={onStopDrum} />
                </>
              )}
              <SecondaryButton label="↻  Refresh Grooves (after adding via USB)" onPress={onRefreshCatalog} />

              <Text style={styles.sectionLabel}>Instrument (kit)</Text>
              {kitsOk ? (
                <>
                  <Dropdown label="Kit" options={drumKits} value={drumKit} onSelect={onSelectDrumKit} />
                  <Stepper count={drumKits.length} value={drumKit} onStep={onSelectDrumKit} />
                </>
              ) : (
                <Text style={styles.dim}>
                  {synth.name} has one fixed rhythm set — GM drum kits (Standard/Room/Power/…) apply only on
                  the SF2/TSF soundfont engines.
                </Text>
              )}

              <StepSlider
                label="Speed"
                value={drumSpeed}
                min={25}
                max={200}
                step={5}
                onPreview={onPreviewDrumSpeed}
                onCommit={onCommitDrumSpeed}
              />
              <StepSlider
                label="Volume"
                value={drumVol}
                min={0}
                max={150}
                step={5}
                onPreview={onPreviewDrumVol}
                onCommit={onCommitDrumVol}
              />
            </>
          )}

          {pane === 'tac5212' && (
            <>
              <Text style={styles.sectionLabel}>Highpass Filter</Text>
              <Text style={styles.dim}>
                Removes low-frequency rumble from the DAC output. Off passes the full range through.
              </Text>
              <View style={{ height: 10 }} />
              <SecondaryButton
                label={hpfOn ? 'Highpass: On' : 'Highpass: Off'}
                onPress={onToggleHpf}
              />

              {hpfOn && (
                <>
                  <Text style={styles.sectionLabel}>Cutoff</Text>
                  <Dropdown label="Cutoff" options={HPF_CUTOFFS} value={hpfCutIdx} onSelect={onSelectHpfCut} />
                </>
              )}
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

// Prev/next stepper (‹ ›) for quickly walking through options without opening the
// dropdown. Wraps around at the ends. Disabled when there are fewer than 2 options.
function Stepper({ count, value, onStep }: { count: number; value: number; onStep: (index: number) => void }) {
  const disabled = count < 2;
  const go = (delta: number) => {
    if (disabled) return;
    onStep((value + delta + count) % count);
  };
  return (
    <View style={styles.stepperRow}>
      <Pressable
        disabled={disabled}
        onPress={() => go(-1)}
        style={({ pressed }) => [styles.stepBtn, disabled && styles.btnDisabled, pressed && styles.btnPressed]}
      >
        <Text style={styles.stepBtnText}>‹</Text>
      </Pressable>
      <Pressable
        disabled={disabled}
        onPress={() => go(1)}
        style={({ pressed }) => [styles.stepBtn, disabled && styles.btnDisabled, pressed && styles.btnPressed]}
      >
        <Text style={styles.stepBtnText}>›</Text>
      </Pressable>
    </View>
  );
}

// A slider with tap −/+ buttons on either side that nudge by `step` and commit
// immediately. onPreview updates the on-screen value live (drag or tap); onCommit
// sends to the device (slide-release or a tap). Used for drum speed / volume.
function StepSlider({
  label,
  unit = '%',
  value,
  min,
  max,
  step,
  onPreview,
  onCommit,
}: {
  label: string;
  unit?: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onPreview: (v: number) => void;
  onCommit: (v: number) => void;
}) {
  const bump = (delta: number) => {
    const v = Math.max(min, Math.min(max, value + delta));
    if (v === value) return;
    onPreview(v);
    onCommit(v);
  };
  return (
    <>
      <Text style={styles.sectionLabel}>
        {label} — {value}
        {unit}
      </Text>
      <View style={styles.adjustRow}>
        <Pressable
          onPress={() => bump(-step)}
          style={({ pressed }) => [styles.adjBtn, value <= min && styles.btnDisabled, pressed && styles.btnPressed]}
          disabled={value <= min}
        >
          <Text style={styles.adjBtnText}>−</Text>
        </Pressable>
        <Slider
          style={styles.adjSlider}
          minimumValue={min}
          maximumValue={max}
          step={step}
          value={value}
          onValueChange={onPreview}
          onSlidingComplete={onCommit}
          minimumTrackTintColor="#238636"
          maximumTrackTintColor="#30363d"
          thumbTintColor="#3fb950"
        />
        <Pressable
          onPress={() => bump(step)}
          style={({ pressed }) => [styles.adjBtn, value >= max && styles.btnDisabled, pressed && styles.btnPressed]}
          disabled={value >= max}
        >
          <Text style={styles.adjBtnText}>+</Text>
        </Pressable>
      </View>
    </>
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
  stepperRow: { flexDirection: 'row', gap: 10, marginTop: 10 },
  stepBtn: { flex: 1, backgroundColor: '#21262d', borderRadius: 12, paddingVertical: 12, alignItems: 'center' },
  stepBtnText: { color: '#e6edf3', fontSize: 22, fontWeight: '700' },
  adjustRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  adjSlider: { flex: 1, height: 40 },
  adjBtn: { width: 48, backgroundColor: '#21262d', borderRadius: 12, paddingVertical: 10, alignItems: 'center' },
  adjBtnText: { color: '#e6edf3', fontSize: 24, fontWeight: '700' },
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
