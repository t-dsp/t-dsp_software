// transportFactory.native.ts — native build resolves to this (Metro picks .native.ts).
import type { Transport, TransportKind } from './transport';
import { BleTransport } from './transport.native';
import { WiFiTransport } from './transport.wifi';

// 'default' -> BLE (how the app has always connected). 'wifi' -> LAN WebSocket to the
// ESP32 (firmware built with -D TDSP_CTRL_WIFI). `target` is an optional host/IP/ws-URL;
// omit it to use tdsp.local. NOTE: Android often can't resolve .local — pass the IP there.
export function createTransport(kind: TransportKind = 'default', target?: string): Transport {
  return kind === 'wifi' ? new WiFiTransport(target) : new BleTransport();
}
