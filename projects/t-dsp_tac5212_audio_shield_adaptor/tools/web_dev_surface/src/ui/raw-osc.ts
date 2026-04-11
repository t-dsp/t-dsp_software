// Free-form OSC input field. The dispatcher's typed setters cover the common
// mixer gestures; this exists for ad-hoc pokes — anything in the address tree
// that doesn't have a UI widget yet, plus probing of new firmware-side handlers.
//
// Two input syntaxes are accepted:
//
//   1. Auto-typed (most common, matches the dotted-path CLI's argument inference):
//        /ch/01/mix/fader 0.5
//        /ch/01/mix/on 1
//        /codec/tac5212/adc/1/mode differential
//      The parser infers types from token shape: integers -> i, decimals -> f,
//      everything else -> s.
//
//   2. Explicitly typed (matches OSC type-tag notation):
//        /ch/01/mix/fader f 0.5
//        /sub s addSub i 1000 s /meters/input
//      Use this when auto-inference would pick the wrong type (e.g. when you
//      want "5" to be a float, or when you need T/F/N/I).
//
// Detection: if the second token is a single character from [ifsbTFNI],
// explicit mode kicks in. Otherwise it's auto.

import { Dispatcher } from '../dispatcher';
import { OscArg } from '../osc';

export function rawOsc(
  dispatcher: Dispatcher,
  log: (line: string) => void,
): HTMLElement {
  const root = document.createElement('div');
  root.className = 'raw-osc';

  const input = document.createElement('input');
  input.type = 'text';
  input.placeholder = '/ch/01/mix/fader 0.5';
  input.className = 'raw-osc-input';

  const btn = document.createElement('button');
  btn.textContent = 'Send';

  function fire(): void {
    const text = input.value.trim();
    if (!text) return;
    try {
      const { address, types, args } = parseLine(text);
      dispatcher.sendRaw(address, types, args);
      log(`> ${text}`);
    } catch (e) {
      log(`! ${(e as Error).message}`);
    }
  }

  btn.addEventListener('click', fire);
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') fire();
  });

  root.append(input, btn);
  return root;
}

function parseLine(line: string): { address: string; types: string; args: OscArg[] } {
  const tokens = line.split(/\s+/);
  const address = tokens[0];
  if (!address.startsWith('/')) {
    throw new Error('address must start with /');
  }

  const rest = tokens.slice(1);
  if (rest.length === 0) {
    return { address, types: '', args: [] };
  }

  // Explicit-typed mode if the first arg-position token is a single OSC type tag.
  const explicit = rest[0].length === 1 && /^[ifsbTFNI]$/.test(rest[0]);

  if (explicit) {
    return parseExplicit(address, rest);
  }
  return parseAuto(address, rest);
}

function parseExplicit(
  address: string,
  rest: string[],
): { address: string; types: string; args: OscArg[] } {
  let i = 0;
  let types = '';
  const args: OscArg[] = [];
  while (i < rest.length) {
    const t = rest[i];
    if (!/^[ifsbTFNI]$/.test(t)) {
      throw new Error(`bad type tag '${t}' at position ${i}`);
    }
    types += t;
    if (t === 'T') {
      args.push(true);
      i += 1;
      continue;
    }
    if (t === 'F') {
      args.push(false);
      i += 1;
      continue;
    }
    if (t === 'N' || t === 'I') {
      args.push(null);
      i += 1;
      continue;
    }
    const v = rest[i + 1];
    if (v === undefined) throw new Error(`missing value for type '${t}'`);
    if (t === 'i') {
      args.push(parseInt(v, 10));
    } else if (t === 'f') {
      args.push(parseFloat(v));
    } else if (t === 's') {
      args.push(v);
    } else if (t === 'b') {
      // hex blob: "0a1b2c..." or "0x0a1b..."
      const hex = v.replace(/^0x/, '');
      if (hex.length % 2 !== 0) throw new Error('blob hex must have even length');
      const out = new Uint8Array(hex.length / 2);
      for (let j = 0; j < out.length; j++) {
        out[j] = parseInt(hex.substr(j * 2, 2), 16);
      }
      args.push(out);
    }
    i += 2;
  }
  return { address, types, args };
}

function parseAuto(
  address: string,
  rest: string[],
): { address: string; types: string; args: OscArg[] } {
  let types = '';
  const args: OscArg[] = [];
  for (const tok of rest) {
    if (/^-?\d+$/.test(tok)) {
      types += 'i';
      args.push(parseInt(tok, 10));
    } else if (/^-?(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?$/.test(tok) || /^-?\d+[eE][+-]?\d+$/.test(tok)) {
      types += 'f';
      args.push(parseFloat(tok));
    } else {
      types += 's';
      args.push(tok);
    }
  }
  return { address, types, args };
}
