// Type declaration so TS resolves `./transportFactory` (Metro picks the .web.ts / .native.ts
// implementation at bundle time; this only provides the shared signature for typechecking).
import type { Transport, TransportKind } from './transport';
export function createTransport(kind?: TransportKind, target?: string): Transport;
