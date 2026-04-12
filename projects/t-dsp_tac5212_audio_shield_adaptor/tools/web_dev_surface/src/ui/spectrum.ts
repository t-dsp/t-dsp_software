// Full-content-area stereo spectrum analyzer view.
//
// Subscribes to /spectrum/main (a 1024-byte uint8 dB blob — 512 L bins
// then 512 R bins) when the tab is active, and renders the two traces
// at 60 fps on a Canvas 2D surface. The data rate from the firmware is
// ~30 Hz; the client's rAF loop is independent and always reads the
// latest bins, with EMA smoothing and peak-hold decay applied locally.
//
// Design notes:
// - Data format: uint8 per bin, encoded as clamp((dB + 80) * 255/80).
//   Byte 0 = -80 dB (floor), byte 255 = 0 dB (top of scale). Mapping
//   to y is just `y = (1 - byte/255) * h` — no decode into floats.
// - Log frequency x axis: bin i covers i * (44100/1024) ≈ 43.07 Hz,
//   so bin 0 is DC. We display 20 Hz .. 20 kHz; anything below bin 0
//   (unreachable) or above bin 464 (20 kHz) is clipped at the edges.
// - Colors: L channel cool (blue→magenta), R channel warm (green→
//   yellow→red). Drawn with globalCompositeOperation='lighter' so
//   overlapping bright bands add to white — visually useful when both
//   channels are hot in the same band.
// - Canvas uses device-pixel-ratio backing; all drawing is in CSS
//   pixel coordinates after a one-time ctx.scale(dpr, dpr).
// - Freeze: pauses the rAF loop and leaves whatever was last drawn on
//   screen. Unfreeze resumes.

// Uses its own requestAnimationFrame loop rather than the shared
// raf-batch helper (which coalesces Signal-triggered DOM writes). The
// spectrum view is always animating at 60 fps regardless of data
// cadence, so it needs a dedicated loop.

export interface SpectrumView {
  element: HTMLElement;
  update(bytes: Uint8Array): void;
  start(): void;
  stop(): void;
  freeze(): void;
  unfreeze(): void;
}

const FFT_BINS    = 512;            // per channel
const SAMPLE_RATE = 44100;
const FFT_SIZE    = 1024;           // AudioAnalyzeFFT1024
const BIN_HZ      = SAMPLE_RATE / FFT_SIZE;  // ≈ 43.07 Hz
// Displayed frequency range. F_MIN is set to one bin below BIN_HZ so
// that bin 1 (the first usable FFT bin, at ~43 Hz) maps very close to
// x=0 on the log scale. Earlier we used F_MIN=20, which left an ugly
// blank strip from 20..43 Hz on the left because FFT1024 at 44.1 kHz
// simply has no bins in that range — the display was reserving space
// for data we can never produce.
const F_MIN       = 40;
const F_MAX       = 20000;

// EMA decay factor — higher = slower decay. 0.82 gives ~300 ms to 10%.
const EMA_DECAY = 0.82;

// Peak-hold drop rate in byte units per frame. At 60 fps one byte
// per frame is ~80/255 * 60 dB/s ≈ 19 dB/s — a slow, readable decay.
const PEAK_DROP_PER_FRAME = 1.0;

// Pink-flat tilt: a +3 dB/octave slope compensation applied to the
// displayed bin values (NOT to the raw data). Most real-world audio
// has a natural 1/f "pink" spectrum where power falls ~3 dB/octave;
// pro analyzers like Voxengo SPAN offer this to make pink content
// display as flat, which makes mix balance easier to eyeball. The
// tilt is reversible — click the button again to see the raw
// un-tilted spectrum.
const TILT_DB_PER_OCTAVE = 3.0;
const TILT_REF_HZ        = 1000;  // 0 dB offset at 1 kHz

// Byte-scale factor: our dB-byte mapping is 80 dB over 255 bytes,
// so 1 dB = 255/80 ≈ 3.1875 bytes.
const BYTE_PER_DB        = 255 / 80;

// Bars mode: render the spectrum as N vertical bars log-spaced
// across the frequency range, each showing the max level of the
// FFT bins that fall in its band. Matches the look of the channel
// meters on the mixer view — each bar uses the same green→yellow
// →red peak gradient as the mixer's .meter-fill.peak.
//
// 128 bars = ~13 per octave across the 9-octave display range.
// Fine detail, still cheap (~512 fillRects/frame; Canvas 2D
// handles this trivially). The low end will show some visible
// stepping because FFT1024's 43 Hz bin spacing means the first
// ~dozen bars below 200 Hz all fall back to the same handful of
// bins via barBinFallback — that's a resolution limit of the
// underlying FFT, not a renderer cost. Going higher than 128
// adds more visual stepping without more real information.
const N_BARS = 128;

const LOG_F_MIN = Math.log(F_MIN);
const LOG_F_MAX = Math.log(F_MAX);
const LOG_F_SPAN = LOG_F_MAX - LOG_F_MIN;

export function spectrumView(): SpectrumView {
  const root = document.createElement('section');
  root.className = 'spectrum-wrap';

  const canvas = document.createElement('canvas');
  canvas.className = 'spectrum-canvas';
  root.appendChild(canvas);

  // Control strip at top-right of the spectrum wrap. Holds Freeze
  // and the display toggles (tilt). Positioned absolute in CSS so it
  // overlays the canvas without affecting canvas layout.
  const controls = document.createElement('div');
  controls.className = 'spectrum-controls';

  const freezeBtn = document.createElement('button');
  freezeBtn.className = 'spectrum-btn freeze-btn';
  freezeBtn.textContent = 'Freeze';

  const tiltBtn = document.createElement('button');
  tiltBtn.className = 'spectrum-btn tilt-btn';
  tiltBtn.title = '+3 dB/octave display tilt — pink content shows as flat';
  tiltBtn.textContent = 'Tilt';

  const barsBtn = document.createElement('button');
  barsBtn.className = 'spectrum-btn bars-btn';
  barsBtn.title = 'Bars mode — log-spaced frequency bands, one bar each';
  barsBtn.textContent = 'Bars';

  controls.append(freezeBtn, tiltBtn, barsBtn);
  root.appendChild(controls);

  const ctx = canvas.getContext('2d');
  if (!ctx) throw new Error('spectrum: 2d context unavailable');

  // Per-bin state. `raw*` is the latest received from the wire;
  // `smoothed*` is the displayed (EMA-decayed) value; `peakHold*` is
  // the dropping-line peak marker. All in dB-byte units 0..255.
  const rawL      = new Uint8Array(FFT_BINS);
  const rawR      = new Uint8Array(FFT_BINS);
  const smoothedL = new Float32Array(FFT_BINS);
  const smoothedR = new Float32Array(FFT_BINS);
  const peakHoldL = new Float32Array(FFT_BINS);
  const peakHoldR = new Float32Array(FFT_BINS);

  // Display-side buffers — the values the draw functions actually
  // read. When tilt is off, these are straight copies from smoothed/
  // peakHold. When tilt is on, each bin has its per-bin tilt offset
  // added (and clamped to 0..255). Keeping the tilt out of the
  // smoothed state means toggling the button is instant and
  // reversible — no retroactive mangling of the EMA state.
  const displayL     = new Float32Array(FFT_BINS);
  const displayR     = new Float32Array(FFT_BINS);
  const displayPeakL = new Float32Array(FFT_BINS);
  const displayPeakR = new Float32Array(FFT_BINS);

  // Per-bin tilt offset in byte units. tiltOffset[i] = 3 * log2(f/1000)
  // converted from dB to bytes. Computed once at init — depends only
  // on FFT bin frequencies, which are fixed constants.
  const tiltOffset = new Float32Array(FFT_BINS);
  for (let i = 0; i < FFT_BINS; i++) {
    const f = i * BIN_HZ;
    if (f <= 0) { tiltOffset[i] = 0; continue; }
    const octaves = Math.log2(f / TILT_REF_HZ);
    tiltOffset[i] = TILT_DB_PER_OCTAVE * octaves * BYTE_PER_DB;
  }

  // Bar-to-bin mapping for bars mode. For each bar, the contiguous
  // range of FFT bins [barBinStart..barBinEnd] whose center
  // frequency falls into this bar's log-spaced frequency band.
  // barBinStart === -1 when no bins fall in the range (narrow
  // low-frequency bars where the bar is smaller than one FFT bin);
  // in that case drawBars uses barBinFallback, the nearest bin to
  // the bar's center frequency.
  const barBinStart    = new Int32Array(N_BARS);
  const barBinEnd      = new Int32Array(N_BARS);
  const barBinFallback = new Int32Array(N_BARS);
  for (let b = 0; b < N_BARS; b++) {
    const tLo = b / N_BARS;
    const tHi = (b + 1) / N_BARS;
    const fLo = Math.exp(LOG_F_MIN + tLo * LOG_F_SPAN);
    const fHi = Math.exp(LOG_F_MIN + tHi * LOG_F_SPAN);
    let first = -1;
    let last  = -1;
    for (let i = 1; i < FFT_BINS; i++) {
      const f = i * BIN_HZ;
      if (f >= fLo && f < fHi) {
        if (first === -1) first = i;
        last = i;
      }
    }
    barBinStart[b] = first;
    barBinEnd[b]   = last;
    const fMid = Math.exp(LOG_F_MIN + (b + 0.5) / N_BARS * LOG_F_SPAN);
    barBinFallback[b] = Math.max(
      1,
      Math.min(FFT_BINS - 1, Math.round(fMid / BIN_HZ)),
    );
  }

  // Bar X positions on the canvas. Depend on cssWidth so
  // recomputed in recomputeBinLayout() on every resize.
  const barLeftX  = new Float32Array(N_BARS);
  const barRightX = new Float32Array(N_BARS);

  let cssWidth  = 0;
  let cssHeight = 0;
  let dpr       = 1;
  let rafId     = 0;
  let frozen    = false;
  let running   = false;
  // Default to tilt on — most real audio is pink, so flat-displaying
  // pink content is the most useful starting point for eyeballing
  // mix balance. Click Tilt to see the raw un-tilted spectrum.
  let tiltOn    = true;
  tiltBtn.classList.add('active');
  // Default to bars mode — closer to the mixer's meter aesthetic
  // and generally easier to read at a glance than the continuous
  // trace. Click the Bars button to toggle back to trace mode.
  let barsOn    = true;
  barsBtn.classList.add('active');

  // Precomputed x coordinates for each bin so the render loop doesn't
  // call Math.log per frame per bin. Rebuilt on resize.
  let binX = new Float32Array(FFT_BINS);
  // Whether this bin is within the displayed frequency range.
  let binVisible = new Uint8Array(FFT_BINS);

  function recomputeBinLayout(): void {
    for (let i = 0; i < FFT_BINS; i++) {
      const f = i * BIN_HZ;
      if (f < F_MIN || f > F_MAX) {
        binVisible[i] = 0;
        binX[i] = 0;
        continue;
      }
      binVisible[i] = 1;
      const t = (Math.log(f) - LOG_F_MIN) / LOG_F_SPAN;
      binX[i] = t * cssWidth;
    }
    // Bar X positions mirror the same log-space mapping so bar
    // edges line up with where the underlying bins are drawn in
    // trace mode — useful when flipping between modes.
    for (let b = 0; b < N_BARS; b++) {
      barLeftX[b]  = (b / N_BARS) * cssWidth;
      barRightX[b] = ((b + 1) / N_BARS) * cssWidth;
    }
  }

  function resize(): void {
    // clientWidth/Height give the padding-box (content + padding,
    // excludes border). The canvas is position:absolute with inset:0
    // inside .spectrum-wrap, so it fills exactly this box. Don't use
    // getBoundingClientRect() — that returns the border-box and would
    // overshoot by the wrap's 1px border on each side, which used to
    // create a ResizeObserver feedback loop that grew the canvas
    // forever.
    const w = Math.max(1, root.clientWidth);
    const h = Math.max(1, root.clientHeight);
    // Bail if unchanged to avoid unnecessary context resets (and to
    // stop any residual ResizeObserver chatter from re-entering).
    const nextDpr = window.devicePixelRatio || 1;
    if (w === cssWidth && h === cssHeight && nextDpr === dpr) return;
    cssWidth  = w;
    cssHeight = h;
    dpr       = nextDpr;
    // Only the backing store is set from JS. Display size is driven
    // by the CSS `inset: 0` rule, so no canvas.style.width/height
    // writes — those were the ones feeding back into the wrap's
    // layout via the old flow-child setup.
    canvas.width  = Math.floor(w * dpr);
    canvas.height = Math.floor(h * dpr);
    ctx!.setTransform(1, 0, 0, 1, 0, 0);
    ctx!.scale(dpr, dpr);
    recomputeBinLayout();
  }

  const ro = new ResizeObserver(() => resize());
  ro.observe(root);

  // --- wire input ---------------------------------------------------

  function update(bytes: Uint8Array): void {
    if (bytes.length < FFT_BINS * 2) return;
    // Copy in — the incoming Uint8Array is a view into the OSC decode
    // buffer which may be reused on the next frame.
    for (let i = 0; i < FFT_BINS; i++) rawL[i] = bytes[i];
    for (let i = 0; i < FFT_BINS; i++) rawR[i] = bytes[FFT_BINS + i];
  }

  // --- color helpers ------------------------------------------------
  //
  // Both channels map the smoothed byte value (0..255) to a hue-ish
  // gradient. Returned as an rgb() string — cheap enough at 60 fps for
  // ~512 values per frame, but we batch the fill into a single path so
  // this is actually called once per channel for the gradient, NOT per
  // bin. Per-bin we just draw a path-segment in one fill color.
  //
  // Actually for the filled-area rendering we use a single gradient
  // computed once per frame per channel, rather than per-bin colors.
  // Peak hold is drawn in plain white.

  function coolGradient(): CanvasGradient {
    // Blue at bottom -> magenta at top, semi-transparent.
    const g = ctx!.createLinearGradient(0, cssHeight, 0, 0);
    g.addColorStop(0.00, 'rgba(40,  80, 200, 0.05)');
    g.addColorStop(0.40, 'rgba(80, 120, 240, 0.55)');
    g.addColorStop(0.80, 'rgba(200, 80, 240, 0.85)');
    g.addColorStop(1.00, 'rgba(240, 120, 240, 0.95)');
    return g;
  }

  function warmGradient(): CanvasGradient {
    // Green at bottom -> yellow -> red near top.
    const g = ctx!.createLinearGradient(0, cssHeight, 0, 0);
    g.addColorStop(0.00, 'rgba( 60, 180,  60, 0.05)');
    g.addColorStop(0.40, 'rgba(100, 220,  80, 0.55)');
    g.addColorStop(0.70, 'rgba(240, 220,  80, 0.80)');
    g.addColorStop(1.00, 'rgba(240,  80,  60, 0.95)');
    return g;
  }

  // Exact stops from the mixer's .meter-fill.peak CSS rule
  // (linear-gradient(to top, #4a4 0%, #4a4 60%, #cc4 75%, #c44 90%)).
  // Used by bars mode so the spectrum bars match the look of the
  // channel meters on the mixer page. Both L and R channels use
  // this same gradient and overlay via `lighter` compositing —
  // identical mono content shows a single bright band, and stereo
  // imbalance shows as slightly brighter / dimmer regions.
  function mixerMeterGradient(): CanvasGradient {
    const g = ctx!.createLinearGradient(0, cssHeight, 0, 0);
    g.addColorStop(0.00, '#4a4');
    g.addColorStop(0.60, '#4a4');
    g.addColorStop(0.75, '#cc4');
    g.addColorStop(0.90, '#c44');
    return g;
  }

  // --- render loop --------------------------------------------------

  function drawGridlines(): void {
    const c = ctx!;
    c.save();
    c.globalCompositeOperation = 'source-over';
    c.strokeStyle = 'rgba(255, 255, 255, 0.08)';
    c.fillStyle   = 'rgba(200, 200, 200, 0.55)';
    c.lineWidth   = 1;
    c.font        = '10px ui-monospace, Consolas, monospace';
    c.textBaseline = 'bottom';

    // Vertical frequency gridlines.
    const freqs = [100, 1000, 10000];
    const labels = ['100 Hz', '1 kHz', '10 kHz'];
    c.beginPath();
    for (let i = 0; i < freqs.length; i++) {
      const f = freqs[i];
      const x = Math.floor(((Math.log(f) - LOG_F_MIN) / LOG_F_SPAN) * cssWidth) + 0.5;
      c.moveTo(x, 0);
      c.lineTo(x, cssHeight);
    }
    c.stroke();
    c.textAlign = 'left';
    for (let i = 0; i < freqs.length; i++) {
      const f = freqs[i];
      const x = Math.floor(((Math.log(f) - LOG_F_MIN) / LOG_F_SPAN) * cssWidth);
      c.fillText(labels[i], x + 4, cssHeight - 4);
    }

    // Horizontal dB gridlines.
    const dBs = [-20, -40, -60];
    c.beginPath();
    for (const db of dBs) {
      const byte = (db + 80) * (255 / 80);
      const y = Math.floor((1 - byte / 255) * cssHeight) + 0.5;
      c.moveTo(0, y);
      c.lineTo(cssWidth, y);
    }
    c.stroke();
    c.textAlign = 'left';
    for (const db of dBs) {
      const byte = (db + 80) * (255 / 80);
      const y = Math.floor((1 - byte / 255) * cssHeight);
      c.fillText(`${db} dB`, 4, y - 2);
    }
    c.restore();
  }

  function drawTrace(
    smoothed: Float32Array,
    fillStyle: CanvasGradient,
  ): void {
    const c = ctx!;
    c.beginPath();
    // Start at bottom-left to close the filled area.
    let started = false;
    for (let i = 0; i < FFT_BINS; i++) {
      if (!binVisible[i]) continue;
      const x = binX[i];
      const y = (1 - smoothed[i] / 255) * cssHeight;
      if (!started) {
        c.moveTo(x, cssHeight);
        c.lineTo(x, y);
        started = true;
      } else {
        c.lineTo(x, y);
      }
    }
    if (!started) return;
    // Close the polygon along the bottom.
    // Find the rightmost visible bin x so the close returns to baseline
    // under the last drawn point.
    let rightX = 0;
    for (let i = FFT_BINS - 1; i >= 0; i--) {
      if (binVisible[i]) { rightX = binX[i]; break; }
    }
    c.lineTo(rightX, cssHeight);
    c.closePath();
    c.fillStyle = fillStyle;
    c.fill();
  }

  function drawPeakHold(peak: Float32Array, stroke: string): void {
    const c = ctx!;
    c.save();
    c.globalCompositeOperation = 'source-over';
    c.strokeStyle = stroke;
    c.lineWidth = 1;
    c.beginPath();
    let started = false;
    for (let i = 0; i < FFT_BINS; i++) {
      if (!binVisible[i]) continue;
      const x = binX[i];
      const y = (1 - peak[i] / 255) * cssHeight;
      if (!started) {
        c.moveTo(x, y);
        started = true;
      } else {
        c.lineTo(x, y);
      }
    }
    c.stroke();
    c.restore();
  }

  // Pick the max display value across the FFT bins that belong to
  // bar `b`. Falls back to the nearest bin if the bar's frequency
  // range is smaller than one bin (happens at the low end where
  // bars are narrow and bins are 43 Hz wide).
  function barMax(b: number, buf: Float32Array): number {
    const start = barBinStart[b];
    if (start === -1) return buf[barBinFallback[b]];
    const end = barBinEnd[b];
    let m = 0;
    for (let i = start; i <= end; i++) {
      if (buf[i] > m) m = buf[i];
    }
    return m;
  }

  function drawBars(display: Float32Array, fillStyle: CanvasGradient): void {
    const c = ctx!;
    c.fillStyle = fillStyle;
    const barGap = 2;
    for (let b = 0; b < N_BARS; b++) {
      const v = barMax(b, display);
      if (v < 1) continue;
      const x = barLeftX[b];
      const w = Math.max(1, barRightX[b] - barLeftX[b] - barGap);
      const y = (1 - v / 255) * cssHeight;
      const h = cssHeight - y;
      c.fillRect(x, y, w, h);
    }
  }

  function drawBarPeaks(peak: Float32Array, stroke: string): void {
    const c = ctx!;
    c.save();
    c.globalCompositeOperation = 'source-over';
    c.fillStyle = stroke;
    const barGap = 2;
    for (let b = 0; b < N_BARS; b++) {
      const v = barMax(b, peak);
      if (v < 1) continue;
      const x = barLeftX[b];
      const w = Math.max(1, barRightX[b] - barLeftX[b] - barGap);
      const y = Math.floor((1 - v / 255) * cssHeight);
      // 2-px peak tick on top of each bar, like the peak indicator
      // above the channel meters on the mixer view.
      c.fillRect(x, y - 1, w, 2);
    }
    c.restore();
  }

  function frame(): void {
    if (!running) return;
    rafId = requestAnimationFrame(frame);
    if (frozen) return;

    // Advance EMA + peak hold from whatever the latest raw values are.
    for (let i = 0; i < FFT_BINS; i++) {
      const rl = rawL[i];
      let sl = smoothedL[i] * EMA_DECAY;
      if (rl > sl) sl = rl;
      smoothedL[i] = sl;

      let pl = peakHoldL[i] - PEAK_DROP_PER_FRAME;
      if (pl < 0) pl = 0;
      if (rl > pl) pl = rl;
      peakHoldL[i] = pl;

      const rr = rawR[i];
      let sr = smoothedR[i] * EMA_DECAY;
      if (rr > sr) sr = rr;
      smoothedR[i] = sr;

      let pr = peakHoldR[i] - PEAK_DROP_PER_FRAME;
      if (pr < 0) pr = 0;
      if (rr > pr) pr = rr;
      peakHoldR[i] = pr;
    }

    // Populate display buffers. Straight copy if tilt is off, per-bin
    // offset-and-clamp if on. Tilt is applied to both the filled
    // smoothed trace AND the peak-hold line so they stay consistent
    // with each other. Clamp to 0..255 so the draw functions don't
    // see out-of-range values (a +3 dB/oct tilt can push bins above
    // 255 at the top of the audible range, and pull bins below 0 at
    // the bottom).
    if (tiltOn) {
      for (let i = 0; i < FFT_BINS; i++) {
        const o = tiltOffset[i];
        let vl = smoothedL[i] + o;
        if (vl < 0) vl = 0;
        else if (vl > 255) vl = 255;
        displayL[i] = vl;

        let vr = smoothedR[i] + o;
        if (vr < 0) vr = 0;
        else if (vr > 255) vr = 255;
        displayR[i] = vr;

        let pl = peakHoldL[i] + o;
        if (pl < 0) pl = 0;
        else if (pl > 255) pl = 255;
        displayPeakL[i] = pl;

        let pr = peakHoldR[i] + o;
        if (pr < 0) pr = 0;
        else if (pr > 255) pr = 255;
        displayPeakR[i] = pr;
      }
    } else {
      for (let i = 0; i < FFT_BINS; i++) {
        displayL[i]     = smoothedL[i];
        displayR[i]     = smoothedR[i];
        displayPeakL[i] = peakHoldL[i];
        displayPeakR[i] = peakHoldR[i];
      }
    }

    const c = ctx!;
    c.globalCompositeOperation = 'source-over';
    c.fillStyle = '#0a0a10';
    c.fillRect(0, 0, cssWidth, cssHeight);

    drawGridlines();

    // Both channels use 'lighter' so overlapping hot bands add to
    // white, whether we're in trace or bars mode.
    c.globalCompositeOperation = 'lighter';

    if (barsOn) {
      // Both channels use the mixer's peak gradient; compute it
      // once per frame. `lighter` compositing means L+R overlap
      // brightens — with identical channels you see one bright
      // slab, with stereo imbalance you see the brighter side
      // pop above the dimmer one.
      const meterGrad = mixerMeterGradient();
      drawBars(displayL, meterGrad);
      drawBars(displayR, meterGrad);
      // Peak-hold ticks in a dim white — subtle but visible over
      // both green bars and the red upper stops of the gradient.
      drawBarPeaks(displayPeakL, 'rgba(255, 255, 255, 0.75)');
      drawBarPeaks(displayPeakR, 'rgba(255, 255, 255, 0.75)');
    } else {
      drawTrace(displayL, coolGradient());
      drawTrace(displayR, warmGradient());
      drawPeakHold(displayPeakL, 'rgba(200, 200, 255, 0.7)');
      drawPeakHold(displayPeakR, 'rgba(255, 220, 200, 0.7)');
    }
  }

  function start(): void {
    if (running) return;
    running = true;
    resize();
    rafId = requestAnimationFrame(frame);
  }

  function stop(): void {
    running = false;
    if (rafId) cancelAnimationFrame(rafId);
    rafId = 0;
  }

  function freeze(): void {
    frozen = true;
    freezeBtn.classList.add('active');
    freezeBtn.textContent = 'Frozen';
  }

  function unfreeze(): void {
    frozen = false;
    freezeBtn.classList.remove('active');
    freezeBtn.textContent = 'Freeze';
  }

  freezeBtn.addEventListener('click', () => {
    if (frozen) unfreeze();
    else freeze();
  });

  tiltBtn.addEventListener('click', () => {
    tiltOn = !tiltOn;
    tiltBtn.classList.toggle('active', tiltOn);
  });

  barsBtn.addEventListener('click', () => {
    barsOn = !barsOn;
    barsBtn.classList.toggle('active', barsOn);
  });

  return { element: root, update, start, stop, freeze, unfreeze };
}
