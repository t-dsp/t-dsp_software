// Primitive cell builders shared by channel-pair, main-bus, host-strip.
//
// The mixer surface is laid out in 7 fixed-height rows so every strip
// aligns horizontally:
//
//   row 1: name      (.row-name)
//   row 2: meter     (.row-meter)
//   row 3: fader     (.row-fader)
//   row 4: fader-val (.row-fv)
//   row 5: mute      (.row-mute)
//   row 6: solo      (.row-solo)
//   row 7: link      (.row-link)   — link on pairs, enable on host, empty on single strips that don't need it
//
// These helpers build the individual cells. Each wrapper function
// (channelPair / mainBus / hostStrip) calls `makeRow` once per row and
// appends the cells it cares about; spacer cells fill unused slots so
// every wrapper is exactly 7 rows tall at the same heights.

import { Signal } from '../state';
import { rafBatch } from '../raf-batch';
import { formatFaderDb } from './util';

export function makeRow(kind: string): HTMLDivElement {
  const row = document.createElement('div');
  row.className = `strip-row ${kind}`;
  return row;
}

export function makeName(nameSig: Signal<string>): HTMLElement {
  const el = document.createElement('div');
  el.className = 'strip-name cell';
  nameSig.subscribe((n) => (el.textContent = n));
  return el;
}

export function makeStaticName(text: string): HTMLElement {
  const el = document.createElement('div');
  el.className = 'strip-name cell';
  el.textContent = text;
  return el;
}

// Meter-floor (dB) for the visual scale. Anything at or below this
// reads as 0% fill; 0 dBFS reads as 100%. A 60 dB range gives mic-
// level signals (~-40 dBFS) a visible ~33% bar instead of the
// near-invisible 1% you'd get from a linear 0..1 map.
const METER_FLOOR_DB = -60;

// Returns 0..1 (NOT 0..100) — used as the scaleY factor on the
// transform-based meter fill. The fill div is sized to the full
// meter height and `scaleY(n)` shrinks it from the bottom (set
// transform-origin: bottom in CSS).
function levelToScale(v: number): number {
  if (v <= 0) return 0;
  const db = 20 * Math.log10(v);
  if (db <= METER_FLOOR_DB) return 0;
  if (db >= 0) return 1;
  return (db - METER_FLOOR_DB) / -METER_FLOOR_DB;
}

export function makeMeter(peak: Signal<number>, rms: Signal<number>): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'meter-wrap cell';
  const peakFill = document.createElement('div');
  peakFill.className = 'meter-fill peak';
  const rmsFill = document.createElement('div');
  rmsFill.className = 'meter-fill rms';
  wrap.append(rmsFill, peakFill);
  // Use transform: scaleY instead of height — transforms are
  // compositor-only (no layout, no paint, no style recalc on the
  // meter subtree). transform-origin is set to `bottom` in style.css.
  //
  // RAF-batched: rapid Signal updates (e.g. multiple meter blobs in
  // one task) collapse to one DOM write per frame, keyed by the fill
  // element so each meter writes independently. Caps DOM throughput
  // at ~60 writes/sec/meter regardless of how often the firmware
  // streams blobs. Stores the latest value in a closure variable so
  // the deferred updater always uses the freshest reading.
  let pendingPeak = 0;
  let pendingRms = 0;
  peak.subscribe((p) => {
    pendingPeak = p;
    rafBatch(peakFill, () => {
      peakFill.style.transform = `scaleY(${levelToScale(pendingPeak)})`;
    });
  });
  rms.subscribe((r) => {
    pendingRms = r;
    rafBatch(rmsFill, () => {
      rmsFill.style.transform = `scaleY(${levelToScale(pendingRms)})`;
    });
  });
  return wrap;
}

export function makeMeterSpacer(): HTMLElement {
  const s = document.createElement('div');
  s.className = 'meter-wrap meter-spacer cell';
  return s;
}

export function makeFader(
  faderSig: Signal<number>,
  onInput: (v: number) => void,
): HTMLInputElement {
  const f = document.createElement('input');
  f.type = 'range';
  f.className = 'fader cell';
  f.min = '0';
  f.max = '1';
  f.step = '0.001';
  faderSig.subscribe((v) => (f.value = String(v)));
  f.addEventListener('input', () => onInput(parseFloat(f.value)));
  return f;
}

export function makeDisabledFader(faderSig: Signal<number>): HTMLInputElement {
  const f = document.createElement('input');
  f.type = 'range';
  f.className = 'fader cell';
  f.min = '0';
  f.max = '1';
  f.step = '0.001';
  f.disabled = true;
  faderSig.subscribe((v) => (f.value = String(v)));
  return f;
}

export function makeFaderValue(faderSig: Signal<number>): HTMLElement {
  const el = document.createElement('div');
  el.className = 'fader-value cell';
  faderSig.subscribe((v) => (el.textContent = formatFaderDb(v)));
  return el;
}

export function makeMute(
  onSig: Signal<boolean>,
  onClick: () => void,
  opts: { title?: string; label?: string } = {},
): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = 'mute cell';
  b.textContent = opts.label ?? 'M';
  if (opts.title) b.title = opts.title;
  onSig.subscribe((on) => b.classList.toggle('active', !on));
  b.addEventListener('click', onClick);
  return b;
}

export function makeSolo(
  soloSig: Signal<boolean>,
  onClick: () => void,
): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = 'solo cell';
  b.textContent = 'S';
  b.title = 'Solo (SIP)';
  soloSig.subscribe((s) => b.classList.toggle('active', s));
  b.addEventListener('click', onClick);
  return b;
}

// A transparent element matching the vertical height of whatever cell
// class it emulates. Used in rows that would otherwise be empty, to
// keep the row at the same height as its peers in other wrappers.
export function makeCellSpacer(kind: 'mute' | 'solo' | 'link' | 'fv' | 'name'): HTMLElement {
  const s = document.createElement('div');
  s.className = `cell cell-spacer spacer-${kind}`;
  return s;
}
