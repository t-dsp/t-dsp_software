// deviceNetwork.web.ts — see deviceNetwork.d.ts. The web build never holds the device network password:
// a page that can talk to the T-DSP is already on its network (or on the same LAN).
import type { DeviceNetworkCreds } from './deviceNetwork.d';

export async function loadDeviceNetworkCreds(): Promise<DeviceNetworkCreds | null> { return null; }
export async function rememberDeviceNetworkCreds(_ssid: string, _pass: string): Promise<void> {}
export const canOpenWifiSettings = false;
export async function openWifiSettings(): Promise<boolean> { return false; }
