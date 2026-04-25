// biquad-widget.ts — single-band biquad EQ row.
//
// Renders: type select | frequency slider | gain slider | Q slider, plus a
// shared canvas response curve (the parent EQ section owns the canvas; this
// widget just publishes the current coefs and a redraw hook).
//
// On any control change:
//   1. Compute coefs locally (so the response curve repaints instantly).
//   2. Send the design + coefs over OSC to firmware:
//        addressBase + '/design'  s f f f  (type, freqHz, gainDb, Q)
//        addressBase + '/coeffs'  iiiii    (raw 5.27 coefs — backup path
//                                           if the firmware echoes /design
//                                           we use that, otherwise the
//                                           coeffs will arrive separately)
//
// On firmware echo of /design or /coeffs, update local state and redraw —
// keeps multiple clients in sync if they're both editing.

import { Dispatcher } from '../dispatcher';
import { Signal } from '../state';
import { OscMessage } from '../osc';
import { BiquadDesign, BiquadType } from '../codec-panel-config';
import { BiquadCoeffs, design as designBiquad } from '../biquad-design';

// Sample rate assumed by the dev surface for response-curve previews and
// outbound coefficient calculation. The firmware also assumes 48 kHz when
// designing from a /design message — see Tac5212Panel::handleDacBiquadDesign.
const FS_HZ = 48000;

const TYPE_OPTIONS: BiquadType[] = [
  'off', 'peak', 'low_shelf', 'high_shelf', 'low_pass', 'high_pass', 'band_pass', 'notch',
];

const TYPE_LABELS: Record<BiquadType, string> = {
  'off':         'Off',
  'peak':        'Peak',
  'low_shelf':   'Low Shelf',
  'high_shelf':  'High Shelf',
  'low_pass':    'Low Pass',
  'high_pass':   'High Pass',
  'band_pass':   'Band Pass',
  'notch':       'Notch',
};

// Some types don't use gain (LP/HP/BP/Notch). Grey out the gain slider
// when the type is one of those.
function typeUsesGain(t: BiquadType): boolean {
  return t === 'peak' || t === 'low_shelf' || t === 'high_shelf';
}

export interface BiquadBandHandle {
  element: HTMLElement;
  // Current coefficients — kept in sync as user edits or firmware echoes.
  coefs: Signal<BiquadCoeffs>;
  // Current design (type/freq/gain/Q).
  design: Signal<BiquadDesign>;
}

export function biquadBand(
  label: string,
  addressBase: string,
  defaults: BiquadDesign,
  dispatcher: Dispatcher,
  onChanged: () => void,   // parent calls back to repaint composite curve
): BiquadBandHandle {
  const wrap = document.createElement('div');
  wrap.className = 'biquad-band';

  const labelEl = document.createElement('label');
  labelEl.className = 'biquad-band-label';
  labelEl.textContent = label;
  wrap.appendChild(labelEl);

  // --- type select ---
  const typeSel = document.createElement('select');
  typeSel.className = 'biquad-type';
  for (const t of TYPE_OPTIONS) {
    const o = document.createElement('option');
    o.value = t;
    o.textContent = TYPE_LABELS[t];
    typeSel.appendChild(o);
  }
  typeSel.value = defaults.type;
  wrap.appendChild(typeSel);

  // --- frequency slider (log scale) ---
  // Slider 0..1000 maps to 20 Hz .. 20 kHz on a log scale. The on-screen
  // readout shows the actual Hz/kHz value. Log mapping makes high Q's
  // worth of bandwidth visible at low end and prevents the slider from
  // being unusable above 1 kHz.
  const FREQ_MIN_HZ = 20;
  const FREQ_MAX_HZ = 20000;
  const SLIDER_RES  = 1000;
  function freqToSlider(hz: number): number {
    const lo = Math.log(FREQ_MIN_HZ);
    const hi = Math.log(FREQ_MAX_HZ);
    return Math.round((Math.log(Math.max(hz, FREQ_MIN_HZ)) - lo) / (hi - lo) * SLIDER_RES);
  }
  function sliderToFreq(s: number): number {
    const lo = Math.log(FREQ_MIN_HZ);
    const hi = Math.log(FREQ_MAX_HZ);
    return Math.exp(lo + (s / SLIDER_RES) * (hi - lo));
  }

  const freqSlider = document.createElement('input');
  freqSlider.type = 'range';
  freqSlider.min = '0';
  freqSlider.max = String(SLIDER_RES);
  freqSlider.step = '1';
  freqSlider.value = String(freqToSlider(defaults.freqHz));
  freqSlider.className = 'biquad-freq';
  wrap.appendChild(freqSlider);

  const freqRead = document.createElement('span');
  freqRead.className = 'biquad-readout';
  wrap.appendChild(freqRead);

  // --- gain slider ---
  const gainSlider = document.createElement('input');
  gainSlider.type = 'range';
  gainSlider.min = '-12';
  gainSlider.max = '12';
  gainSlider.step = '0.5';
  gainSlider.value = String(defaults.gainDb);
  gainSlider.className = 'biquad-gain';
  wrap.appendChild(gainSlider);

  const gainRead = document.createElement('span');
  gainRead.className = 'biquad-readout';
  wrap.appendChild(gainRead);

  // --- Q slider ---
  const qSlider = document.createElement('input');
  qSlider.type = 'range';
  qSlider.min = '0.3';
  qSlider.max = '6';
  qSlider.step = '0.05';
  qSlider.value = String(defaults.q);
  qSlider.className = 'biquad-q';
  wrap.appendChild(qSlider);

  const qRead = document.createElement('span');
  qRead.className = 'biquad-readout';
  wrap.appendChild(qRead);

  // --- state ---
  const designSig: Signal<BiquadDesign> = new Signal<BiquadDesign>({ ...defaults });
  const coefsSig: Signal<BiquadCoeffs> = new Signal<BiquadCoeffs>(
    designBiquad(defaults.type, FS_HZ, defaults.freqHz, defaults.gainDb, defaults.q));

  function formatHz(hz: number): string {
    if (hz >= 1000) return `${(hz / 1000).toFixed(hz >= 10000 ? 1 : 2)} kHz`;
    return `${hz.toFixed(0)} Hz`;
  }

  function refreshReadouts(d: BiquadDesign): void {
    freqRead.textContent = formatHz(d.freqHz);
    gainRead.textContent = typeUsesGain(d.type) ? `${d.gainDb >= 0 ? '+' : ''}${d.gainDb.toFixed(1)} dB` : '—';
    qRead.textContent    = `Q ${d.q.toFixed(2)}`;
    gainSlider.disabled = !typeUsesGain(d.type);
  }

  function pushDesign(): void {
    const d = designSig.get();
    // Local coefficient computation (mirrors firmware) so the curve
    // refreshes instantly without a round-trip.
    const c = designBiquad(d.type, FS_HZ, d.freqHz, d.gainDb, d.q);
    coefsSig.set(c);
    onChanged();
    // Ship to firmware. /design is the convenient form; the firmware also
    // emits a /coeffs echo back so distant clients can resync.
    dispatcher.sendRawThrottled(`${addressBase}/design`, 'sfff', [d.type, d.freqHz, d.gainDb, d.q]);
  }

  // Emit on any input.
  typeSel.addEventListener('change', () => {
    const d = { ...designSig.get(), type: typeSel.value as BiquadType };
    designSig.set(d);
    refreshReadouts(d);
    pushDesign();
  });
  freqSlider.addEventListener('input', () => {
    const hz = sliderToFreq(parseFloat(freqSlider.value));
    const d = { ...designSig.get(), freqHz: hz };
    designSig.set(d);
    refreshReadouts(d);
    pushDesign();
  });
  gainSlider.addEventListener('input', () => {
    const d = { ...designSig.get(), gainDb: parseFloat(gainSlider.value) };
    designSig.set(d);
    refreshReadouts(d);
    pushDesign();
  });
  qSlider.addEventListener('input', () => {
    const d = { ...designSig.get(), q: parseFloat(qSlider.value) };
    designSig.set(d);
    refreshReadouts(d);
    pushDesign();
  });

  // Listen for firmware echoes:
  //   /design echo (s f f f) → sync sliders
  //   /coeffs echo (i i i i i) → sync curve directly (covers the "raw coef
  //   write" case where someone POSTs /coeffs without going through /design)
  dispatcher.registerCodecListener(`${addressBase}/design`, (msg: OscMessage) => {
    if (msg.types !== 'sfff') return;
    const t = msg.args[0] as BiquadType;
    const fHz = msg.args[1] as number;
    const dB = msg.args[2] as number;
    const q  = msg.args[3] as number;
    if (typeof fHz !== 'number') return;
    const d: BiquadDesign = { type: t, freqHz: fHz, gainDb: dB, q };
    designSig.set(d);
    refreshReadouts(d);
    if (typeSel.value !== d.type) typeSel.value = d.type;
    freqSlider.value = String(freqToSlider(d.freqHz));
    gainSlider.value = String(d.gainDb);
    qSlider.value    = String(d.q);
    coefsSig.set(designBiquad(d.type, FS_HZ, d.freqHz, d.gainDb, d.q));
    onChanged();
  });
  dispatcher.registerCodecListener(`${addressBase}/coeffs`, (msg: OscMessage) => {
    if (msg.types !== 'iiiii') return;
    const c: BiquadCoeffs = {
      n0: msg.args[0] as number,
      n1: msg.args[1] as number,
      n2: msg.args[2] as number,
      d1: msg.args[3] as number,
      d2: msg.args[4] as number,
    };
    coefsSig.set(c);
    onChanged();
  });

  // Initial paint.
  refreshReadouts(designSig.get());

  return { element: wrap, coefs: coefsSig, design: designSig };
}

// EQ canvas — shared by all bands of a section. The widget owns the canvas;
// individual bands call `repaint` after publishing new coefs.
export interface EqCurveHandle {
  element: HTMLElement;
  setBands: (bands: readonly BiquadBandHandle[]) => void;
  repaint: () => void;
}

export function eqCurve(): EqCurveHandle {
  const wrap = document.createElement('div');
  wrap.className = 'biquad-curve-wrap';
  const canvas = document.createElement('canvas');
  canvas.className = 'biquad-curve';
  canvas.width = 600;
  canvas.height = 140;
  wrap.appendChild(canvas);

  let bands: readonly BiquadBandHandle[] = [];

  function repaint(): void {
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    // Grid: dB lines at -12, -6, 0, +6, +12; freq decades.
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    ctx.lineWidth = 1;
    const dBLines = [-12, -6, 0, 6, 12];
    function dBToY(dB: number): number {
      return h * (1 - (dB + 14) / 28);   // ±14 dB visible range
    }
    for (const dB of dBLines) {
      const y = dBToY(dB);
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }
    // 0 dB highlighted.
    ctx.strokeStyle = 'rgba(255,255,255,0.18)';
    {
      const y = dBToY(0);
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }

    // Decade markers.
    const fLo = 20;
    const fHi = 20000;
    const lLo = Math.log10(fLo);
    const lHi = Math.log10(fHi);
    function fToX(f: number): number {
      return ((Math.log10(f) - lLo) / (lHi - lLo)) * w;
    }
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    for (let dec = 0; dec < 4; ++dec) {
      const f = Math.pow(10, lLo + dec);
      for (let m = 1; m < 10; ++m) {
        const fx = f * m;
        if (fx < fLo || fx > fHi) continue;
        const x = fToX(fx);
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
        ctx.stroke();
      }
    }

    // Cascade response.
    if (bands.length > 0) {
      ctx.strokeStyle = 'hsl(40, 90%, 60%)';
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      const N = w;
      const coefsArr = bands.map((b) => b.coefs.get());
      for (let i = 0; i <= N; ++i) {
        const f = Math.pow(10, lLo + (i / N) * (lHi - lLo));
        // Sum dB across bands, mirroring magnitudeDbCascade.
        let dB = 0;
        for (const c of coefsArr) {
          // Inline the magnitude-dB to avoid an import cycle here.
          // Recover textbook coefs.
          const b0 = (c.n0 | 0) * (1 / (1 << 27));
          const b1 = (c.n1 | 0) * (1 / (1 << 27)) * 2;
          const b2 = (c.n2 | 0) * (1 / (1 << 27));
          const a1 = -(c.d1 | 0) * (1 / (1 << 27)) * 2;
          const a2 = -(c.d2 | 0) * (1 / (1 << 27));
          const w0 = 2 * Math.PI * f / FS_HZ;
          const cw = Math.cos(w0);
          const sw = Math.sin(w0);
          const c2 = Math.cos(2 * w0);
          const s2 = Math.sin(2 * w0);
          const numRe = b0 + b1 * cw + b2 * c2;
          const numIm =     -b1 * sw - b2 * s2;
          const denRe = 1 + a1 * cw + a2 * c2;
          const denIm =    -a1 * sw - a2 * s2;
          const numMag = Math.hypot(numRe, numIm);
          const denMag = Math.hypot(denRe, denIm);
          if (denMag <= 1e-30) continue;
          const m = numMag / denMag;
          if (m > 1e-10) dB += 20 * Math.log10(m);
        }
        const x = i;
        const y = dBToY(dB);
        if (i === 0) ctx.moveTo(x, y);
        else         ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
  }

  return {
    element: wrap,
    setBands: (b) => { bands = b; repaint(); },
    repaint,
  };
}
