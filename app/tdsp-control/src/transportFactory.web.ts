// transportFactory.web.ts — desktop build resolves to this (Metro picks .web.ts).
import type { Transport } from './transport';
import { WebSerialTransport } from './transport.web';

export function createTransport(): Transport { return new WebSerialTransport(); }
