// Type declaration so TS resolves `./transportFactory` (Metro picks the .web.ts / .native.ts
// implementation at bundle time; this only provides the shared signature for typechecking).
import type { Transport } from './transport';
export function createTransport(): Transport;
