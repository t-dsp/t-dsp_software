// tdspBle.ts — BLE control protocol + React hook for the T-DSP receiver.
// ---------------------------------------------------------------------------
// Mirrors the firmware contract in
// projects/t-dsp_esp32_bt_receiver/src/main.cpp:
//   Service  7a9c0001-...  "T-DSP Control"
//   Command  7a9c0002-...  WRITE        (1 byte opcode)
//   Status   7a9c0003-...  READ+NOTIFY  (small JSON: {"conn":,"disc":,"peer":})
//
// react-native-ble-plx requires a custom dev/preview build (NOT Expo Go): BLE is
// a native module. Build via EAS (`eas build -p android --profile preview`).

import { useCallback, useEffect, useRef, useState } from 'react';
import { PermissionsAndroid, Platform } from 'react-native';
import { BleManager, Device, State, Subscription } from 'react-native-ble-plx';

export const TDSP_SVC_UUID = '7a9c0001-4a6e-4b7d-8f1a-2d3c4e5f6a70';
export const TDSP_CMD_UUID = '7a9c0002-4a6e-4b7d-8f1a-2d3c4e5f6a70';
export const TDSP_STAT_UUID = '7a9c0003-4a6e-4b7d-8f1a-2d3c4e5f6a70';
export const TDSP_SRC_UUID = '7a9c0004-4a6e-4b7d-8f1a-2d3c4e5f6a70';
// Device catalog: '|'-delimited name lists the firmware serves so the app renders
// its Dexed pickers dynamically (no app update when songs/instruments change).
export const TDSP_SONGS_UUID = '7a9c0005-4a6e-4b7d-8f1a-2d3c4e5f6a70';
export const TDSP_INSTR_UUID = '7a9c0006-4a6e-4b7d-8f1a-2d3c4e5f6a70';

// Command opcodes — must match the firmware enum.
export const CMD = {
  PAIRING_MODE: 0x01, // become discoverable + connectable
  END_PAIRING: 0x02, // leave pairing mode
  DISCONNECT: 0x03, // drop the current A2DP source
  FORGET: 0x04, // forget the last paired device
  RECONNECT: 0x05, // reconnect A2DP to the last paired phone
  SET_VOLUME: 0x10, // + 1 byte: master volume 0..100 (%)
  CONNECT_ADDR: 0x11, // + 6 bytes BD address: switch A2DP to that paired phone
  FORGET_ADDR: 0x12, // + 6 bytes BD address: remove that bond
  PLAY_SONG: 0x20, // play the built-in Dexed demo (William Tell)
  STOP_SONG: 0x21, // stop the Dexed demo
  SET_DX_VOICE: 0x22, // + 1 byte: Dexed instrument index (into DX_INSTRUMENTS)
  REFRESH_CAT: 0x23, // re-scan SD + refresh the song/instrument catalog
  SET_HPF: 0x24, // + 1 byte: TAC5212 DAC highpass mode (0=off,1=1Hz,2=12Hz,3=96Hz)
  SET_MIDI_MODE: 0x25, // + 1 byte: 0 = normal MIDI, 1 = MPE (per-note expression)
  SET_LOOP: 0x26, // + 1 byte: 0/1 — loop the current song when it ends
  SET_PRESSURE: 0x27, // + 1 byte: MPE pressure routing bitmask (1=vol 2=bright 4=vib 8=trem)
  SET_MODWHEEL: 0x28, // + 1 byte: mod-wheel routing bitmask (2=bright 4=vib 8=trem)
  SET_LFOMODE: 0x29, // + 1 byte: 0 respect patch LFO / 1 force LFO on any patch
  SET_TIMBRE: 0x2a, // + 1 byte: CC74 timbre (MPE Y) routing bitmask (2=bright 4=vib 8=trem)
} as const;

// TAC5212 DAC highpass filter modes — byte sent via SET_HPF. Maps to the
// tac5212::DacHpf enum on the Teensy (0=Programmable/all-pass = off).
export const HPF_MODE = {
  OFF: 0,
  CUT_1HZ: 1,
  CUT_12HZ: 2,
  CUT_96HZ: 3,
} as const;

// Synth descriptor shown on the MIDI page. The firmware reports the engine it
// was BUILT with (Dexed vs ymfm OPM, …) as an optional header on the instrument
// catalog (see readCatalog); this is the fallback for firmware that predates
// that header, so the UI still reads correctly against an old device.
export type SynthInfo = { name: string; description: string };
export const DEFAULT_SYNTH: SynthInfo = {
  name: 'Dexed',
  description: '6-op FM synth, played by the MIDI IN port and the songs below.',
};

// Dexed instrument list — index sent via SET_DX_VOICE. MUST stay in sync (order
// AND names) with kInstruments[] in
// projects/spike_esp32_bt_spdif_mix_kit/src/main.cpp.
export const DX_INSTRUMENTS = [
  // Keys
  'E.Piano', 'Grand Piano', 'FM Rhodes', 'E.Piano 2', 'Harpsichord', 'Clav', 'Celeste',
  // Organs
  'Organ', 'Pipe Organ',
  // Strings / ensemble
  'Strings', 'String Ens', 'Orchestra', 'Pizzicato',
  // Brass
  'Brass', 'Trumpet', 'Synth Brass',
  // Winds
  'Flute', 'Pan Flute', 'Oboe', 'Clarinet', 'Sax', 'Harmonica',
  // Guitar / plucked
  'Guitar', 'Jazz Guitar', 'Sitar', 'Harp',
  // Bass
  'Bass', 'E.Bass', 'Fretless',
  // Synth / lead
  'Syn Lead', 'Mini Moog', 'Jupiter 8', 'Synclavier',
  // Mallets / bells / perc
  'Vibes', 'Marimba', 'Xylophone', 'Tub Bells', 'Glockenspiel', 'Steel Drum', 'Timpani',
  // Voice
  'Voice', 'Choir',
] as const;

// Standard General MIDI program names (0..127). A GM engine (SF2 / TSF / OPL3)
// doesn't stream its instrument names — the firmware just flags "GM" on the catalog
// header (a 3rd \t-field) and the app renders these standard names locally. This
// avoids streaming ~2 KB of names over the UART/BLE (a BLE characteristic caps at
// 512 B, so only ~30 names would otherwise fit). See sendCatalog() / readCatalog().
export const GM_INSTRUMENTS = [
  'Acoustic Grand Piano', 'Bright Acoustic Piano', 'Electric Grand Piano', 'Honky-tonk Piano',
  'Electric Piano 1', 'Electric Piano 2', 'Harpsichord', 'Clavinet',
  'Celesta', 'Glockenspiel', 'Music Box', 'Vibraphone', 'Marimba', 'Xylophone', 'Tubular Bells', 'Dulcimer',
  'Drawbar Organ', 'Percussive Organ', 'Rock Organ', 'Church Organ', 'Reed Organ',
  'Accordion', 'Harmonica', 'Tango Accordion',
  'Acoustic Guitar (nylon)', 'Acoustic Guitar (steel)', 'Electric Guitar (jazz)', 'Electric Guitar (clean)',
  'Electric Guitar (muted)', 'Overdriven Guitar', 'Distortion Guitar', 'Guitar Harmonics',
  'Acoustic Bass', 'Electric Bass (finger)', 'Electric Bass (pick)', 'Fretless Bass',
  'Slap Bass 1', 'Slap Bass 2', 'Synth Bass 1', 'Synth Bass 2',
  'Violin', 'Viola', 'Cello', 'Contrabass', 'Tremolo Strings', 'Pizzicato Strings', 'Orchestral Harp', 'Timpani',
  'String Ensemble 1', 'String Ensemble 2', 'Synth Strings 1', 'Synth Strings 2',
  'Choir Aahs', 'Voice Oohs', 'Synth Voice', 'Orchestra Hit',
  'Trumpet', 'Trombone', 'Tuba', 'Muted Trumpet', 'French Horn', 'Brass Section', 'Synth Brass 1', 'Synth Brass 2',
  'Soprano Sax', 'Alto Sax', 'Tenor Sax', 'Baritone Sax', 'Oboe', 'English Horn', 'Bassoon', 'Clarinet',
  'Piccolo', 'Flute', 'Recorder', 'Pan Flute', 'Blown Bottle', 'Shakuhachi', 'Whistle', 'Ocarina',
  'Lead 1 (square)', 'Lead 2 (sawtooth)', 'Lead 3 (calliope)', 'Lead 4 (chiff)',
  'Lead 5 (charang)', 'Lead 6 (voice)', 'Lead 7 (fifths)', 'Lead 8 (bass + lead)',
  'Pad 1 (new age)', 'Pad 2 (warm)', 'Pad 3 (polysynth)', 'Pad 4 (choir)',
  'Pad 5 (bowed)', 'Pad 6 (metallic)', 'Pad 7 (halo)', 'Pad 8 (sweep)',
  'FX 1 (rain)', 'FX 2 (soundtrack)', 'FX 3 (crystal)', 'FX 4 (atmosphere)',
  'FX 5 (brightness)', 'FX 6 (goblins)', 'FX 7 (echoes)', 'FX 8 (sci-fi)',
  'Sitar', 'Banjo', 'Shamisen', 'Koto', 'Kalimba', 'Bagpipe', 'Fiddle', 'Shanai',
  'Tinkle Bell', 'Agogo', 'Steel Drums', 'Woodblock', 'Taiko Drum', 'Melodic Tom', 'Synth Drum', 'Reverse Cymbal',
  'Guitar Fret Noise', 'Breath Noise', 'Seashore', 'Bird Tweet', 'Telephone Ring', 'Helicopter', 'Applause', 'Gunshot',
] as const;

// Built-in Dexed songs — index sent via PLAY_SONG. MUST stay in sync (order)
// with kSongs[] in projects/spike_esp32_bt_spdif_mix_kit/src/main.cpp.
export const DX_SONGS = [
  'William Tell Overture',
  'Moonlight Sonata (3rd Mvt)',
  'Billie Jean',
  'Bohemian Rhapsody',
] as const;

export type TdspStatus = {
  conn: boolean; // an A2DP source is connected
  disc: boolean; // receiver is discoverable (pairing mode)
  vol: number; // master headphone volume 0..100 (%)
  hpf: number; // TAC5212 DAC highpass mode 0..3 (0=off) — reported by the device
  mpe: boolean; // false = normal MIDI, true = MPE (per-note expression)
  peer: string; // connected source name, if any
};

// One paired phone in the sources list (from the SRC characteristic JSON).
export type TdspSource = {
  a: string; // 12-char BD-address hex (stable id)
  n: string; // friendly name (falls back to the address hex)
  c: boolean; // is this the currently connected source
};

export type ConnState = 'idle' | 'scanning' | 'connecting' | 'connected';

// --- minimal base64 (values are ASCII: 1 opcode byte out, ASCII JSON in) -----
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function bytesToBase64(bytes: number[]): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i];
    const b1 = i + 1 < bytes.length ? bytes[i + 1] : 0;
    const b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
    out += B64[b0 >> 2];
    out += B64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out += i + 2 < bytes.length ? B64[b2 & 63] : '=';
  }
  return out;
}

function base64ToString(b64: string): string {
  let out = '';
  const clean = b64.replace(/[^A-Za-z0-9+/]/g, '');
  for (let i = 0; i < clean.length; i += 4) {
    const n0 = B64.indexOf(clean[i]);
    const n1 = B64.indexOf(clean[i + 1]);
    const n2 = B64.indexOf(clean[i + 2]);
    const n3 = B64.indexOf(clean[i + 3]);
    out += String.fromCharCode((n0 << 2) | (n1 >> 4));
    if (n2 >= 0) out += String.fromCharCode(((n1 & 15) << 4) | (n2 >> 2));
    if (n3 >= 0) out += String.fromCharCode(((n2 & 3) << 6) | n3);
  }
  return out;
}

function parseStatus(raw: string | null | undefined): TdspStatus | null {
  if (!raw) return null;
  try {
    const j = JSON.parse(base64ToString(raw));
    return {
      conn: !!j.conn,
      disc: !!j.disc,
      vol: typeof j.vol === 'number' ? j.vol : 50,
      hpf: typeof j.hpf === 'number' ? j.hpf : 0,
      mpe: !!j.mpe,
      peer: typeof j.peer === 'string' ? j.peer : '',
    };
  } catch {
    return null;
  }
}

// Address hex ("aabbcc…") -> 6 bytes for the CONNECT_ADDR / FORGET_ADDR payload.
function hexToBytes6(hex: string): number[] {
  const out: number[] = [];
  for (let i = 0; i + 1 < hex.length && out.length < 6; i += 2) {
    out.push(parseInt(hex.substr(i, 2), 16) & 0xff);
  }
  while (out.length < 6) out.push(0);
  return out;
}

function parseSources(raw: string | null | undefined): TdspSource[] {
  if (!raw) return [];
  try {
    const arr = JSON.parse(base64ToString(raw));
    if (!Array.isArray(arr)) return [];
    return arr
      .filter((s) => s && typeof s.a === 'string')
      .map((s) => ({ a: s.a as string, n: typeof s.n === 'string' && s.n ? s.n : s.a, c: !!s.c }));
  } catch {
    return [];
  }
}

// Android 12+ needs BLUETOOTH_SCAN/CONNECT at runtime; older needs location.
async function ensurePermissions(): Promise<boolean> {
  if (Platform.OS !== 'android') return true; // iOS prompts via the BLE stack
  const api = typeof Platform.Version === 'number' ? Platform.Version : parseInt(String(Platform.Version), 10);
  try {
    if (api >= 31) {
      const res = await PermissionsAndroid.requestMultiple([
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
      ]);
      return (
        res[PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN] === 'granted' &&
        res[PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT] === 'granted'
      );
    }
    const loc = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION);
    return loc === 'granted';
  } catch {
    return false;
  }
}

export function useTdsp() {
  const managerRef = useRef<BleManager | null>(null);
  const deviceRef = useRef<Device | null>(null);
  const statusSubRef = useRef<Subscription | null>(null);
  const srcSubRef = useRef<Subscription | null>(null);
  const songsSubRef = useRef<Subscription | null>(null);
  const instrSubRef = useRef<Subscription | null>(null);

  const [state, setState] = useState<ConnState>('idle');
  const [status, setStatus] = useState<TdspStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [btReady, setBtReady] = useState(false);
  const [volume, setVolumeState] = useState(50); // slider position 0..100
  const [sources, setSources] = useState<TdspSource[]>([]); // paired phones
  const [catSongs, setCatSongs] = useState<string[]>([]); // songs fetched from device
  const [catInstruments, setCatInstruments] = useState<string[]>([]); // instruments fetched
  const [catSynth, setCatSynth] = useState<SynthInfo | null>(null); // engine the firmware was built with

  // Coalescing volume writer: rapid slider drags collapse to the latest value so
  // we never flood the BLE link; a write always converges to the final position.
  const pendingVolRef = useRef<number | null>(null);
  const writingVolRef = useRef(false);
  const volInitedRef = useRef(false);

  // One BleManager for the app lifetime.
  useEffect(() => {
    const mgr = new BleManager();
    managerRef.current = mgr;
    const sub = mgr.onStateChange((s) => setBtReady(s === State.PoweredOn), true);
    return () => {
      sub.remove();
      statusSubRef.current?.remove();
      srcSubRef.current?.remove();
      songsSubRef.current?.remove();
      instrSubRef.current?.remove();
      deviceRef.current?.cancelConnection().catch(() => {});
      mgr.destroy();
    };
  }, []);

  const disconnect = useCallback(async () => {
    statusSubRef.current?.remove();
    statusSubRef.current = null;
    srcSubRef.current?.remove();
    srcSubRef.current = null;
    songsSubRef.current?.remove();
    songsSubRef.current = null;
    instrSubRef.current?.remove();
    instrSubRef.current = null;
    try {
      await deviceRef.current?.cancelConnection();
    } catch {}
    deviceRef.current = null;
    volInitedRef.current = false;
    setState('idle');
    setStatus(null);
    setSources([]);
    setCatSynth(null); // next device re-reports its own engine
  }, []);

  // Read the full sources list (ATT read-blob returns the whole JSON value).
  const readSources = useCallback(async () => {
    const device = deviceRef.current;
    if (!device) return;
    try {
      const c = await device.readCharacteristicForService(TDSP_SVC_UUID, TDSP_SRC_UUID);
      setSources(parseSources(c.value));
    } catch {}
  }, []);

  // ---- Catalog reassembly (chunked over BLE) --------------------------------
  // A catalog list longer than one BLE value (~512 B) — e.g. Dexed's full 320-
  // voice set — arrives as a burst of framed notifications, each
  // "<seq>\x1e<count>\x1e<payload>" (0x1e = record separator). We reassemble the
  // chunks in order and apply the joined list once all have arrived. A value with
  // no framing (old firmware, or a list that fit in one value) is applied as-is.
  const RS = '\x1e';
  type CatAsm = { count: number; parts: (string | undefined)[]; got: number };
  const songsAsmRef = useRef<CatAsm | null>(null);
  const instrAsmRef = useRef<CatAsm | null>(null);

  const applySongsStr = useCallback((full: string) => {
    const list = full.split('|').filter((x) => x.length > 0);
    if (list.length) setCatSongs(list);
  }, []);

  const applyInstrStr = useCallback((full: string) => {
    // The instrument list may carry an optional synth header as its FIRST
    // '|'-field, so the MIDI page labels itself from the engine the firmware was
    // built with:  "\x1F<name>\t<description>|inst0|inst1|..."  The header is
    // marked by a leading 0x1F (unit separator); a 3rd \t-field "GM" means a
    // General-MIDI engine sends NO names and we render GM_INSTRUMENTS locally.
    const parts = full.split('|').filter((x) => x.length > 0);
    let isGM = false;
    if (parts.length && parts[0].startsWith('\x1f')) {
      const fields = parts.shift()!.slice(1).split('\t');
      const name = (fields[0] ?? '').trim();
      const description = (fields[1] ?? '').trim();
      if ((fields[2] ?? '').trim() === 'GM') isGM = true;
      if (name) setCatSynth({ name, description: description || DEFAULT_SYNTH.description });
    }
    if (isGM) setCatInstruments(GM_INSTRUMENTS as unknown as string[]);
    else if (parts.length) setCatInstruments(parts);
  }, []);

  // Feed one raw characteristic value (base64) into a reassembler; apply the
  // joined list once every chunk of the burst has been collected.
  const feedCatalog = useCallback(
    (
      ref: { current: CatAsm | null },
      value: string | null | undefined,
      apply: (full: string) => void
    ) => {
      const s = value ? base64ToString(value) : '';
      if (!s) return;
      const i1 = s.indexOf(RS);
      const i2 = i1 >= 0 ? s.indexOf(RS, i1 + 1) : -1;
      if (i1 < 0 || i2 < 0) {
        // Unframed value (old firmware / single value) — treat as the whole list.
        ref.current = null;
        apply(s);
        return;
      }
      const seq = parseInt(s.slice(0, i1), 10);
      const count = parseInt(s.slice(i1 + 1, i2), 10);
      const payload = s.slice(i2 + 1);
      if (!Number.isFinite(seq) || !Number.isFinite(count) || count < 1 || seq < 0 || seq >= count) return;
      let asm = ref.current;
      if (!asm || asm.count !== count) {
        asm = { count, parts: new Array(count).fill(undefined), got: 0 };
        ref.current = asm;
      }
      if (asm.parts[seq] === undefined) asm.got++;
      asm.parts[seq] = payload;
      if (asm.got >= count) {
        const full = asm.parts.join('');
        ref.current = null;
        apply(full);
      }
    },
    []
  );

  // Read each catalog characteristic once and feed it into the reassembler. For a
  // single-chunk (or legacy) value this completes immediately; for a multi-chunk
  // burst the notify subscription + a forced refresh deliver the rest.
  const readCatalog = useCallback(async () => {
    const device = deviceRef.current;
    if (!device) return;
    try {
      const c = await device.readCharacteristicForService(TDSP_SVC_UUID, TDSP_SONGS_UUID);
      feedCatalog(songsAsmRef, c.value, applySongsStr);
    } catch {}
    try {
      const c = await device.readCharacteristicForService(TDSP_SVC_UUID, TDSP_INSTR_UUID);
      feedCatalog(instrAsmRef, c.value, applyInstrStr);
    } catch {}
  }, [feedCatalog, applySongsStr, applyInstrStr]);

  const subscribeStatus = useCallback(async (device: Device) => {
    // Bigger MTU so the status JSON fits one notification and the sources list
    // fits one READ (Android; iOS negotiates/long-reads on its own).
    if (Platform.OS === 'android') {
      try {
        await device.requestMTU(512);
      } catch {}
    }
    try {
      const initial = await device.readCharacteristicForService(TDSP_SVC_UUID, TDSP_STAT_UUID);
      const parsed = parseStatus(initial.value);
      if (parsed) setStatus(parsed);
    } catch {}
    statusSubRef.current = device.monitorCharacteristicForService(
      TDSP_SVC_UUID,
      TDSP_STAT_UUID,
      (err, ch) => {
        if (err) return; // disconnect surfaces via onDisconnected below
        const parsed = parseStatus(ch?.value);
        if (parsed) setStatus(parsed);
      }
    );
    // Sources: read once, then re-read on every "changed" notification (the full
    // list can exceed a notification's MTU, so we re-READ rather than parse it).
    await readSources();
    srcSubRef.current = device.monitorCharacteristicForService(
      TDSP_SVC_UUID,
      TDSP_SRC_UUID,
      (err) => {
        if (err) return;
        readSources();
      }
    );
    // Catalog (Dexed songs + instruments): read once, then reassemble the chunked
    // burst the device streams on each "changed" notify (a long list — e.g. the
    // full 320-voice Dexed set — spans several notifications).
    await readCatalog();
    songsSubRef.current = device.monitorCharacteristicForService(
      TDSP_SVC_UUID,
      TDSP_SONGS_UUID,
      (err, ch) => {
        if (!err) feedCatalog(songsAsmRef, ch?.value, applySongsStr);
      }
    );
    instrSubRef.current = device.monitorCharacteristicForService(
      TDSP_SVC_UUID,
      TDSP_INSTR_UUID,
      (err, ch) => {
        if (!err) feedCatalog(instrAsmRef, ch?.value, applyInstrStr);
      }
    );
    // Force a fresh catalog burst now that our notify subscriptions are live: the
    // ESP32's on-connect send can fire before we've subscribed, so its chunks
    // would be lost. Re-request, then retry once if a chunk went missing (an
    // in-flight reassembler that never completed leaves its ref non-null).
    const kickCatalog = () =>
      device
        .writeCharacteristicWithResponseForService(
          TDSP_SVC_UUID,
          TDSP_CMD_UUID,
          bytesToBase64([CMD.REFRESH_CAT])
        )
        .catch(() => {});
    await kickCatalog();
    setTimeout(() => {
      if (songsAsmRef.current || instrAsmRef.current) kickCatalog();
    }, 2500);
  }, [readSources, readCatalog, feedCatalog, applySongsStr, applyInstrStr]);

  const scanAndConnect = useCallback(async () => {
    const mgr = managerRef.current;
    if (!mgr) return;
    setError(null);
    if (!(await ensurePermissions())) {
      setError('Bluetooth permission denied');
      return;
    }
    setState('scanning');
    mgr.startDeviceScan([TDSP_SVC_UUID], null, async (err, device) => {
      if (err) {
        setError(err.message);
        setState('idle');
        return;
      }
      if (!device) return;
      mgr.stopDeviceScan();
      setState('connecting');
      try {
        const connected = await device.connect();
        await connected.discoverAllServicesAndCharacteristics();
        deviceRef.current = connected;
        connected.onDisconnected(() => {
          statusSubRef.current?.remove();
          statusSubRef.current = null;
          srcSubRef.current?.remove();
          srcSubRef.current = null;
          songsSubRef.current?.remove();
          songsSubRef.current = null;
          instrSubRef.current?.remove();
          instrSubRef.current = null;
          deviceRef.current = null;
          volInitedRef.current = false;
          setStatus(null);
          setSources([]);
          setState('idle');
        });
        await subscribeStatus(connected);
        setState('connected');
      } catch (e: any) {
        setError(e?.message ?? 'connect failed');
        setState('idle');
      }
    });
    // Safety: stop scanning after 15 s if nothing is found.
    setTimeout(() => {
      if (deviceRef.current == null) {
        mgr.stopDeviceScan();
        setState((s) => (s === 'scanning' ? 'idle' : s));
      }
    }, 15000);
  }, [subscribeStatus]);

  const sendCommand = useCallback(async (opcode: number) => {
    const device = deviceRef.current;
    if (!device) {
      setError('not connected');
      return;
    }
    try {
      await device.writeCharacteristicWithResponseForService(
        TDSP_SVC_UUID,
        TDSP_CMD_UUID,
        bytesToBase64([opcode & 0xff])
      );
    } catch (e: any) {
      setError(e?.message ?? 'write failed');
    }
  }, []);

  // Command carrying a 6-byte BD address (CONNECT_ADDR / FORGET_ADDR).
  const writeAddrCmd = useCallback(async (opcode: number, addrHex: string) => {
    const device = deviceRef.current;
    if (!device) {
      setError('not connected');
      return;
    }
    try {
      await device.writeCharacteristicWithResponseForService(
        TDSP_SVC_UUID,
        TDSP_CMD_UUID,
        bytesToBase64([opcode & 0xff, ...hexToBytes6(addrHex)])
      );
    } catch (e: any) {
      setError(e?.message ?? 'write failed');
    }
  }, []);

  const connectSource = useCallback(
    (addrHex: string) => writeAddrCmd(CMD.CONNECT_ADDR, addrHex),
    [writeAddrCmd]
  );
  const forgetSource = useCallback(
    (addrHex: string) => writeAddrCmd(CMD.FORGET_ADDR, addrHex),
    [writeAddrCmd]
  );

  // Command carrying a single data byte (e.g. SET_DX_VOICE + instrument index).
  const writeByteCmd = useCallback(async (opcode: number, value: number) => {
    const device = deviceRef.current;
    if (!device) {
      setError('not connected');
      return;
    }
    try {
      await device.writeCharacteristicWithResponseForService(
        TDSP_SVC_UUID,
        TDSP_CMD_UUID,
        bytesToBase64([opcode & 0xff, value & 0xff])
      );
    } catch (e: any) {
      setError(e?.message ?? 'write failed');
    }
  }, []);

  // Dexed (MIDI synth) controls.
  const playSong = useCallback((index: number = 0) => writeByteCmd(CMD.PLAY_SONG, index), [writeByteCmd]);
  const stopSong = useCallback(() => sendCommand(CMD.STOP_SONG), [sendCommand]);
  const setDxVoice = useCallback(
    (index: number) => writeByteCmd(CMD.SET_DX_VOICE, index),
    [writeByteCmd]
  );
  // TAC5212 DAC highpass filter — mode 0=off, 1/2/3 = 1/12/96 Hz cutoff.
  const setHpf = useCallback(
    (mode: number) => writeByteCmd(CMD.SET_HPF, mode),
    [writeByteCmd]
  );
  // Switch the device between normal MIDI and MPE (per-note expression, e.g. LinnStrument).
  const setMidiMode = useCallback(
    (mpe: boolean) => writeByteCmd(CMD.SET_MIDI_MODE, mpe ? 1 : 0),
    [writeByteCmd]
  );
  // Loop the current song when it reaches the end (great for the built-in test sequences).
  const setLoop = useCallback(
    (on: boolean) => writeByteCmd(CMD.SET_LOOP, on ? 1 : 0),
    [writeByteCmd]
  );
  // MPE pressure routing bitmask: 1=volume 2=brightness 4=vibrato 8=tremolo (combine).
  const setPressure = useCallback(
    (mask: number) => writeByteCmd(CMD.SET_PRESSURE, mask & 0xff),
    [writeByteCmd]
  );
  // Mod-wheel routing bitmask: 2=brightness 4=vibrato 8=tremolo (volume bit ignored).
  const setModWheel = useCallback(
    (mask: number) => writeByteCmd(CMD.SET_MODWHEEL, mask & 0xff),
    [writeByteCmd]
  );
  // LFO mode for vibrato/tremolo: false = respect the patch's own LFO, true = force one.
  const setLfoMode = useCallback(
    (force: boolean) => writeByteCmd(CMD.SET_LFOMODE, force ? 1 : 0),
    [writeByteCmd]
  );
  // CC74 timbre (MPE Y-axis / slide) routing bitmask: 2=brightness 4=vibrato 8=tremolo.
  const setTimbre = useCallback(
    (mask: number) => writeByteCmd(CMD.SET_TIMBRE, mask & 0xff),
    [writeByteCmd]
  );
  // Ask the device to re-scan its SD card and re-send the catalog. The device
  // NOTIFYs the songs/instruments chars, which re-reads via the subscription; we
  // also re-read after a short delay in case the notify is missed.
  const refreshCatalog = useCallback(() => {
    sendCommand(CMD.REFRESH_CAT);
    // Re-read several times, staggered: the ESP32->Teensy->ESP32 round trip
    // (which includes an SD re-scan on the Teensy) can easily exceed a single
    // 700ms wait, so one read often fires before the fresh catalog lands. A few
    // spaced reads converge reliably without spamming the BLE link. (The notify
    // subscription also re-reads, but a device that re-sends an unchanged value
    // may not notify, so we don't rely on it.)
    [300, 900, 1800, 3000].forEach((ms) => setTimeout(() => readCatalog(), ms));
  }, [sendCommand, readCatalog]);

  // Drain the pending volume to the device, coalescing bursts into the latest
  // value AND rate-limiting to ~11 writes/sec. The phone shares one radio between
  // BLE and A2DP, so a burst of BLE writes while streaming audio makes the music
  // glitch — pacing the writes keeps the slider responsive without starving A2DP.
  const pumpVolume = useCallback(async () => {
    const device = deviceRef.current;
    if (!device || writingVolRef.current) return;
    writingVolRef.current = true;
    try {
      while (pendingVolRef.current != null) {
        const v = pendingVolRef.current;
        pendingVolRef.current = null;
        try {
          await device.writeCharacteristicWithoutResponseForService(
            TDSP_SVC_UUID,
            TDSP_CMD_UUID,
            bytesToBase64([CMD.SET_VOLUME, v & 0xff])
          );
        } catch (e: any) {
          setError(e?.message ?? 'volume write failed');
        }
        await new Promise((r) => setTimeout(r, 90)); // pace for BLE/A2DP coexistence
      }
    } finally {
      writingVolRef.current = false;
      if (pendingVolRef.current != null) pumpVolumeRef.current?.(); // catch trailing value
    }
  }, []);
  // Stable self-reference so the finally-block re-pump doesn't need pumpVolume in deps.
  const pumpVolumeRef = useRef<typeof pumpVolume | null>(null);
  pumpVolumeRef.current = pumpVolume;

  const setVolume = useCallback(
    (pct: number) => {
      const v = Math.max(0, Math.min(100, Math.round(pct)));
      setVolumeState(v);
      pendingVolRef.current = v;
      pumpVolume();
    },
    [pumpVolume]
  );

  // Sync the slider to the device's reported volume once when a connection's first
  // status arrives; after that the local slider is the source of truth.
  useEffect(() => {
    if (status && !volInitedRef.current) {
      volInitedRef.current = true;
      setVolumeState(status.vol);
    }
  }, [status]);

  return {
    state,
    status,
    error,
    btReady,
    volume,
    sources,
    // Dexed catalog — fetched from the device, falling back to the built-in lists
    // if the firmware is older / hasn't reported yet.
    songs: catSongs.length ? catSongs : (DX_SONGS as unknown as string[]),
    instruments: catInstruments.length ? catInstruments : (DX_INSTRUMENTS as unknown as string[]),
    // Synth engine the connected firmware was built with (Dexed / ymfm OPM / …),
    // reported by the device; falls back to Dexed for firmware without the header.
    synth: catSynth ?? DEFAULT_SYNTH,
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
    setLoop,
    setPressure,
    setModWheel,
    setLfoMode,
    setTimbre,
    refreshCatalog,
  };
}
