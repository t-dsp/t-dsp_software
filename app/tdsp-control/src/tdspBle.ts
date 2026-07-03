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

// Command opcodes — must match the firmware enum.
export const CMD = {
  PAIRING_MODE: 0x01, // become discoverable + connectable
  END_PAIRING: 0x02, // leave pairing mode
  DISCONNECT: 0x03, // drop the current A2DP source
  FORGET: 0x04, // forget the last paired device
} as const;

export type TdspStatus = {
  conn: boolean; // an A2DP source is connected
  disc: boolean; // receiver is discoverable (pairing mode)
  peer: string; // connected source name, if any
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
    return { conn: !!j.conn, disc: !!j.disc, peer: typeof j.peer === 'string' ? j.peer : '' };
  } catch {
    return null;
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

  const [state, setState] = useState<ConnState>('idle');
  const [status, setStatus] = useState<TdspStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [btReady, setBtReady] = useState(false);

  // One BleManager for the app lifetime.
  useEffect(() => {
    const mgr = new BleManager();
    managerRef.current = mgr;
    const sub = mgr.onStateChange((s) => setBtReady(s === State.PoweredOn), true);
    return () => {
      sub.remove();
      statusSubRef.current?.remove();
      deviceRef.current?.cancelConnection().catch(() => {});
      mgr.destroy();
    };
  }, []);

  const disconnect = useCallback(async () => {
    statusSubRef.current?.remove();
    statusSubRef.current = null;
    try {
      await deviceRef.current?.cancelConnection();
    } catch {}
    deviceRef.current = null;
    setState('idle');
    setStatus(null);
  }, []);

  const subscribeStatus = useCallback(async (device: Device) => {
    // Bigger MTU so the JSON status fits one notification (Android only).
    if (Platform.OS === 'android') {
      try {
        await device.requestMTU(185);
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
  }, []);

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
          deviceRef.current = null;
          setStatus(null);
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

  return { state, status, error, btReady, scanAndConnect, disconnect, sendCommand };
}
