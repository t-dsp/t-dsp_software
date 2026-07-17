// transportFactory.web.ts — desktop build resolves to this (Metro picks .web.ts).
import type { Transport, TransportKind } from './transport';
import { WebSerialTransport } from './transport.web';
import { WiFiTransport } from './transport.wifi';

// 'default' -> Web Serial (direct USB CDC to the Teensy). 'wifi' -> LAN WebSocket to the
// ESP32 (firmware built with -D TDSP_CTRL_WIFI). `target` is an optional host/IP/ws-URL;
// omit it to use tdsp.local.
export function createTransport(kind: TransportKind = 'default', target?: string): Transport {
  return kind === 'wifi' ? new WiFiTransport(target) : new WebSerialTransport();
}
