// discovery.ts — find T-DSP devices on the LAN via mDNS/Bonjour.
//
//   native -> discovery.native.ts (react-native-zeroconf: Android NSD / iOS Bonjour)
//   web    -> discovery.web.ts    (unsupported: browsers cannot browse mDNS)
//
// Metro resolves the platform file. This module is TYPES ONLY — importing a runtime value
// from here inside a platform sibling would resolve back to that sibling (see transport.ts
// for the same trap). Import the runtime bits from './discoveryFactory'.
//
// The firmware advertises _tdsp._tcp (our own type, so only real T-DSP units show up —
// _ws._tcp is generic enough that any WebSocket gadget would appear) with TXT records:
//   name  = friendly label (BT_DEVICE_NAME)
//   proto = wire format, "at-line/1"
//   a2dp  = "1"/"0" — whether this build has Bluetooth audio (the TDSP_A2DP gate)

export const TDSP_SERVICE = 'tdsp';   // => _tdsp._tcp

export interface TdspDevice {
  /** mDNS service instance name — stable-ish identity, used as the list key. */
  id: string;
  /** Friendly label for the UI (TXT `name`, else the service/host name). */
  name: string;
  /** Resolved IPv4. This is the whole point: the app never has to ask for an IP, and it
   *  sidesteps any host-resolution quirk since we connect straight to the address. */
  host: string;
  port: number;
  /** From TXT `a2dp` — null when the device didn't say. */
  a2dp: boolean | null;
}

export interface Discovery {
  /** True when this platform can browse mDNS at all (false on web). */
  readonly supported: boolean;
  /** Start browsing. `onChange` fires with the full current list on every change. */
  start(onChange: (devices: TdspDevice[]) => void): void;
  /** Stop browsing and release the scanner. Safe to call when not started. */
  stop(): void;
}
