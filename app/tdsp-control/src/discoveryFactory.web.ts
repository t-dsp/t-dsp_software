// discoveryFactory.web.ts — no mDNS on the web. Browsers cannot browse Bonjour/NSD, and
// there is no polyfill worth pretending about, so this reports unsupported and the UI
// falls back to typing a host (tdsp.local works fine in a desktop browser).
import type { Discovery } from './discovery';

class NoDiscovery implements Discovery {
  readonly supported = false;
  start(onChange: (devices: never[]) => void) { onChange([]); }
  stop() {}
}

export function createDiscovery(): Discovery { return new NoDiscovery(); }
