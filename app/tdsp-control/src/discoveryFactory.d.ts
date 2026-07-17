// Type declaration so TS resolves `./discoveryFactory` (Metro picks the .web.ts /
// .native.ts implementation at bundle time; this only provides the shared signature).
import type { Discovery } from './discovery';
export function createDiscovery(): Discovery;
