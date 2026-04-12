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
const F_MIN       = 20;
const F_MAX       = 20000;

// EMA decay factor — higher = slower decay. 0.82 gives ~300 ms to 10%.
const EMA_DECAY = 0.82;

// Peak-hold drop rate in byte units per frame. At 60 fps one byte
// per frame is ~80/255 * 60 dB/s ≈ 19 dB/s — a slow, readable decay.
const PEAK_DROP_PER_FRAME = 1.0;

const LOG_F_MIN = Math.log(F_MIN);
const LOG_F_MAX = Math.log(F_MAX);
const LOG_F_SPAN = LOG_F_MAX - LOG_F_MIN;

export function spectrumView(): SpectrumView {
  const root = document.createElement('section');
  root.className = 'spectrum-wrap';

  const canvas = document.createElement('canvas');
  canvas.className = 'spectrum-canvas';
  root.appendChild(canvas);

  const freezeBtn = document.createElement('button');
  freezeBtn.className = 'freeze-btn';
  freezeBtn.textContent = 'Freeze';
  root.appendChild(freezeBtn);

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

  let cssWidth  = 0;
  let cssHeight = 0;
  let dpr       = 1;
  let rafId     = 0;
  let frozen    = false;
  let running   = false;

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
  }

  function resize(): void {
    const rect = root.getBoundingClientRect();
    const w = Math.max(1, Math.floor(rect.width));
    const h = Math.max(1, Math.floor(rect.height));
    dpr = window.devicePixelRatio || 1;
    cssWidth  = w;
    cssHeight = h;
    canvas.style.width  = w + 'px';
    canvas.style.height = h + 'px';
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

    const c = ctx!;
    c.globalCompositeOperation = 'source-over';
    c.fillStyle = '#0a0a10';
    c.fillRect(0, 0, cssWidth, cssHeight);

    drawGridlines();

    // Both traces use 'lighter' so overlapping hot bands add to white.
    c.globalCompositeOperation = 'lighter';
    drawTrace(smoothedL, coolGradient());
    drawTrace(smoothedR, warmGradient());

    // Peak hold lines in channel-tinted bright colors.
    drawPeakHold(peakHoldL, 'rgba(200, 200, 255, 0.7)');
    drawPeakHold(peakHoldR, 'rgba(255, 220, 200, 0.7)');
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

  return { element: root, update, start, stop, freeze, unfreeze };
}
