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

// Selectable strip name: tapping the name sets the global selectedChannel
// signal so the bottom-strip Sel indicator and (future) TUNE workspace
// know which input is in focus. Highlighted when this channel is the
// current selection. Sel state is local-only — never echoed to firmware,
// since X32 firmware doesn't track per-client selection either.
export function makeSelectableName(
  nameSig: Signal<string>,
  channelIdx: number,
  selectedSig: Signal<number>,
): HTMLElement {
  const el = document.createElement('button');
  el.className = 'strip-name strip-name-sel cell';
  el.title = 'Tap to select';
  nameSig.subscribe((n) => (el.textContent = n));
  selectedSig.subscribe((s) => el.classList.toggle('sel-active', s === channelIdx));
  el.addEventListener('click', () => selectedSig.set(channelIdx));
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
export function makeCellSpacer(kind: 'mute' | 'solo' | 'link' | 'fv' | 'name' | 'rec' | 'gain'): HTMLElement {
  const s = document.createElement('div');
  s.className = `cell cell-spacer spacer-${kind}`;
  return s;
}

// Round gain knob — the small pot that sits above the fader on each
// XLR input strip. Click+vertical-drag to change value (Shift = fine).
// Mouse wheel also nudges by one step. The dial rotates -135°..+135°
// across the value range so the indicator's vertical position roughly
// matches the underlying fader gesture. Returns a wrapper cell that
// stacks the dial above a small dB readout.
export function makeGainKnob(opts: {
  value: Signal<number>;
  min: number;
  max: number;
  step: number;
  unit: string;
  label: string;          // tooltip prefix, e.g. "XLR 1 analog gain"
  onChange: (v: number) => void;
}): HTMLElement {
  const { value, min, max, step, unit, label, onChange } = opts;
  const wrap = document.createElement('div');
  wrap.className = 'gain-knob-wrap cell';

  const knob = document.createElement('div');
  knob.className = 'gain-knob';

  const dial = document.createElement('div');
  dial.className = 'gain-knob-dial';
  const ind = document.createElement('div');
  ind.className = 'gain-knob-indicator';
  dial.appendChild(ind);
  knob.appendChild(dial);

  const readout = document.createElement('div');
  readout.className = 'gain-knob-value';

  wrap.append(knob, readout);

  const decimals = step >= 1 ? 0 : 1;
  const fmt = (v: number): string => v.toFixed(decimals);
  const apply = (v: number): void => {
    const t = (v - min) / (max - min);
    const angle = -135 + t * 270;
    dial.style.transform = `rotate(${angle}deg)`;
    readout.textContent = fmt(v);
    wrap.title = `${label}: ${fmt(v)} ${unit}`;
  };
  value.subscribe(apply);

  const clamp = (v: number): number => {
    if (v < min) return min;
    if (v > max) return max;
    return Math.round(v / step) * step;
  };
  const commit = (v: number): void => {
    const c = clamp(v);
    if (c === value.get()) return;
    value.set(c);
    onChange(c);
  };

  let dragStartY = 0;
  let dragStartV = 0;
  knob.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    knob.setPointerCapture(e.pointerId);
    dragStartY = e.clientY;
    dragStartV = value.get();
  });
  knob.addEventListener('pointermove', (e) => {
    if (!knob.hasPointerCapture(e.pointerId)) return;
    const dy = dragStartY - e.clientY;
    const range = max - min;
    // 100px of drag covers the full range; Shift = 4× finer for precision.
    const sensitivity = (e.shiftKey ? range / 400 : range / 100);
    commit(dragStartV + dy * sensitivity);
  });
  const release = (e: PointerEvent): void => {
    if (knob.hasPointerCapture(e.pointerId)) knob.releasePointerCapture(e.pointerId);
  };
  knob.addEventListener('pointerup', release);
  knob.addEventListener('pointercancel', release);

  knob.addEventListener('wheel', (e) => {
    e.preventDefault();
    const dir = e.deltaY < 0 ? 1 : -1;
    commit(value.get() + dir * step);
  }, { passive: false });

  return wrap;
}

// REC button: per-channel USB record-send toggle. Red when armed.
// The `loopDisabled` signal is `state.main.loopEnable` — when loop is
// engaged the Rec button greys out and ignores clicks (firmware also
// overrides it, but preventing the send keeps the wire traffic clean).
// The stored recSend state is preserved across loop toggles so the
// button pops back to its prior state when loop goes off.
export function makeRec(
  recSig: Signal<boolean>,
  loopDisabled: Signal<boolean>,
  onClick: () => void,
): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = 'rec-btn cell';
  b.textContent = 'REC';
  b.title = 'Send this channel to USB capture (record)';
  recSig.subscribe((on) => b.classList.toggle('active', on));
  loopDisabled.subscribe((loopOn) => {
    b.classList.toggle('loop-override', loopOn);
    b.disabled = loopOn;
    b.title = loopOn
      ? 'Loop is armed — main mix is being recorded instead'
      : 'Send this channel to USB capture (record)';
  });
  b.addEventListener('click', onClick);
  return b;
}
