// Ambient types for react-native-zeroconf (0.14.x ships none).
//
// Deliberately narrow: only the surface discoveryFactory.native.ts actually uses, typed as
// precisely as is honest. `ZeroconfService` fields are optional because the payload differs
// between Android NSD and iOS Bonjour and between the 'found'/'resolved' events — the
// consumer reads them defensively. A blanket `declare module 'react-native-zeroconf'` would
// silently make the whole thing `any` and hide real mistakes.
declare module 'react-native-zeroconf' {
  export interface ZeroconfService {
    name?: string;
    fullName?: string;
    host?: string;
    port?: number;
    addresses?: string[];
    txt?: Record<string, string>;
  }

  /** 'resolved' is the only event carrying addresses/port; 'found' has just a name. */
  export type ZeroconfEvent = 'start' | 'stop' | 'found' | 'resolved' | 'remove' | 'error' | 'update';

  export default class Zeroconf {
    on(event: 'resolved' | 'update', cb: (service: ZeroconfService) => void): void;
    on(event: 'found' | 'remove', cb: (name: string) => void): void;
    on(event: 'error', cb: (err: unknown) => void): void;
    on(event: ZeroconfEvent, cb: (...args: any[]) => void): void;
    scan(type?: string, protocol?: string, domain?: string): void;
    stop(): void;
    removeDeviceListeners(): void;
    getServices(): Record<string, ZeroconfService>;
  }
}
