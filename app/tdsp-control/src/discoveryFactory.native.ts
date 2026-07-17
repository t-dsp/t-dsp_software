// discoveryFactory.native.ts — mDNS browse via react-native-zeroconf (Android NSD /
// iOS Bonjour). Metro picks this on native.
import Zeroconf from 'react-native-zeroconf';
import { TDSP_SERVICE } from './discovery';
import type { Discovery, TdspDevice } from './discovery';

// Zeroconf hands us a service object once RESOLVED (it has addresses/port/txt by then).
// Shape varies a little across platforms, so read defensively.
function toDevice(name: string, s: any): TdspDevice | null {
  if (!s) return null;
  // Prefer an IPv4 address: the ESP32 is v4-only, and a v6 literal would need bracketing
  // in the ws:// URL. `host` is a .local name — we deliberately use the ADDRESS instead so
  // connecting never depends on the platform resolving mDNS names.
  const addrs: string[] = Array.isArray(s.addresses) ? s.addresses : [];
  const ip = addrs.find(a => /^\d+\.\d+\.\d+\.\d+$/.test(a));
  if (!ip || !s.port) return null;
  const txt = s.txt || {};
  const a2dp = txt.a2dp === '1' ? true : txt.a2dp === '0' ? false : null;
  return { id: name, name: txt.name || s.name || name, host: ip, port: s.port, a2dp };
}

class ZeroconfDiscovery implements Discovery {
  readonly supported = true;
  private zc: Zeroconf | null = null;
  private found = new Map<string, TdspDevice>();
  private cb: ((d: TdspDevice[]) => void) | null = null;

  private emit() { this.cb?.(Array.from(this.found.values())); }

  start(onChange: (devices: TdspDevice[]) => void) {
    this.stop();
    this.cb = onChange;
    this.found.clear();
    const zc = new Zeroconf();
    this.zc = zc;
    // 'resolved' is the only event with an address; 'found' fires earlier with just a name.
    zc.on('resolved', (s: any) => {
      const d = toDevice(s?.name ?? '', s);
      if (d) { this.found.set(d.id, d); this.emit(); }
    });
    zc.on('remove', (name: string) => { if (this.found.delete(name)) this.emit(); });
    zc.on('error', (e: any) => console.warn('[tdsp] zeroconf error:', e));
    try { zc.scan(TDSP_SERVICE, 'tcp', 'local.'); }
    catch (e) { console.warn('[tdsp] zeroconf scan failed:', e); }
    this.emit();   // publish the (empty) initial list so the UI can show "scanning…"
  }

  stop() {
    const zc = this.zc;
    this.zc = null; this.cb = null;
    if (!zc) return;
    try { zc.stop(); } catch {}
    try { zc.removeDeviceListeners(); } catch {}
  }
}

export function createDiscovery(): Discovery { return new ZeroconfDiscovery(); }
