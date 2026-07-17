// transportFactory.native.ts — native build resolves to this (Metro picks .native.ts).
import type { Transport } from './transport';
import { BleTransport } from './transport.native';

export function createTransport(): Transport { return new BleTransport(); }
