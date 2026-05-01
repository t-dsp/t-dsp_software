// 88-key piano — display + click-to-play. Renders an A0..C8 keyboard,
// highlights keys as /midi/note events arrive from the firmware (either
// from the USB host port or echoed back from our own click-sends), and
// forwards pointer gestures on the keys to the owner via `onPress(note,
// down)` so main.ts can turn them into /midi/note/in messages.
//
// Display wire format: /midi/note i i i  →  note, velocity, channel.
// velocity == 0 means note-off (standard MIDI running-status idiom),
// so the sink treats (velocity > 0) as "key pressed".
//
// White keys are flex-1 items in a row; black keys are absolutely
// positioned as a percentage of keyboard width, so the layout scales
// with the viewport without needing a ResizeObserver.
//
// Pointer handling lives on the keyboard container (not per-key) so a
// press-and-drag glides across keys like a glissando — each key the
// pointer enters is pressed, each key it leaves is released. We capture
// the pointer on the keyboard and resolve "which key is under the
// cursor" via document.elementFromPoint, which respects z-order so
// black keys (absolutely stacked on top) win over the white key behind
// them when the cursor is in their footprint.

export interface KeyboardView {
  element: HTMLElement;
  // channel is the MIDI channel (1..16) the event arrived on. Omit when
  // the source has no channel (click-to-play UI echoes do pass 1).
  setNote(note: number, on: boolean, channel?: number): void;
  clear(): void;
  start(): void;
  stop(): void;
  isRunning(): boolean;
  // Called on pointerdown (down=true) and pointerup/cancel/leave
  // (down=false) with the MIDI note number of the affected key.
  // Pair registration is one-time — only the most recent handler wins.
  onPress(cb: (note: number, down: boolean) => void): void;
}

const FIRST_NOTE = 21;   // A0
const LAST_NOTE  = 108;  // C8

function isBlackKey(note: number): boolean {
  const pc = ((note % 12) + 12) % 12;
  return pc === 1 || pc === 3 || pc === 6 || pc === 8 || pc === 10;
}

export function keyboardView(): KeyboardView {
  const root = document.createElement('section');
  root.className = 'piano-wrap';

  const banner = document.createElement('div');
  banner.className = 'piano-banner';
  const bannerText = document.createElement('span');
  bannerText.className = 'piano-banner-text';
  bannerText.textContent = 'Click keys to play, or use a USB MIDI keyboard plugged into the Teensy host port.';
  // Live readout of which MIDI channels currently have held notes. Handy
  // for diagnosing why an arp/split/zone isn't triggering a synth: if the
  // arp shows up as a different channel than the synth is listening to,
  // the mismatch is visible right next to the keys lighting up.
  const bannerChannels = document.createElement('span');
  bannerChannels.className = 'piano-banner-channels';
  bannerChannels.textContent = 'MIDI ch: —';
  banner.append(bannerText, bannerChannels);

  const keyboard = document.createElement('div');
  keyboard.className = 'piano-keyboard';

  const whitesRow  = document.createElement('div');
  whitesRow.className = 'piano-whites';
  const blacksLayer = document.createElement('div');
  blacksLayer.className = 'piano-blacks';

  const whites: number[] = [];
  for (let n = FIRST_NOTE; n <= LAST_NOTE; n++) {
    if (!isBlackKey(n)) whites.push(n);
  }
  const whiteCount = whites.length;

  const keyByNote = new Map<number, HTMLElement>();

  // A key is visually lit if EITHER source is holding it. Tracking the
  // two sources independently (rather than a single .active toggle) so
  // that if a user clicks a key while an external MIDI keyboard is also
  // holding the same note, releasing the click doesn't dark the key
  // while external is still held (and vice versa). Each setter syncs
  // the class via syncVisual().
  const pointerHeld = new Set<number>();
  const echoHeld    = new Set<number>();

  // Per-channel held-note sets. Populated from setNote(...) when a
  // channel is supplied. Drives the banner channel readout.
  const heldByChannel = new Map<number, Set<number>>();

  function syncVisual(note: number): void {
    const el = keyByNote.get(note);
    if (!el) return;
    el.classList.toggle('active', pointerHeld.has(note) || echoHeld.has(note));
  }

  function syncChannelReadout(): void {
    if (heldByChannel.size === 0) {
      bannerChannels.textContent = 'MIDI ch: —';
      return;
    }
    const chans = Array.from(heldByChannel.keys()).sort((a, b) => a - b);
    bannerChannels.textContent = `MIDI ch: ${chans.join(', ')}`;
  }

  let pressCb: ((note: number, down: boolean) => void) | null = null;

  function pressNote(note: number): void {
    if (pointerHeld.has(note)) return;
    pointerHeld.add(note);
    syncVisual(note);
    if (pressCb) pressCb(note, true);
  }
  function releaseNote(note: number): void {
    if (!pointerHeld.has(note)) return;
    pointerHeld.delete(note);
    syncVisual(note);
    if (pressCb) pressCb(note, false);
  }

  // Resolve which piano key is under a given viewport point, or null if
  // the cursor is off the keyboard or between the keyboard and a key's
  // hit area. elementFromPoint returns the topmost painted element, so
  // black keys correctly win over the white key they visually cover.
  function noteAtPoint(clientX: number, clientY: number): number | null {
    const el = document.elementFromPoint(clientX, clientY) as HTMLElement | null;
    if (!el) return null;
    const key = el.closest('.piano-key') as HTMLElement | null;
    if (!key) return null;
    const noteStr = key.dataset.note;
    if (!noteStr) return null;
    const n = parseInt(noteStr, 10);
    return Number.isFinite(n) ? n : null;
  }

  // Single active pointer drives the drag. Multitouch presses beyond
  // the first are ignored for now — the drag model is a single glide.
  let activePointerId: number | null = null;
  // activeKey is the note we've currently emitted a note-on for (the
  // wire/visual state). `pointermove` keeps this in sync with the key
  // under the cursor, subject to the rate limit below.
  let activeKey: number | null = null;
  let latestX = 0;
  let latestY = 0;

  // Rate-limit drag transitions to stay comfortably under the
  // serial-bridge throttle (serial-bridge.mjs TX_GAP_MS = 20 ms, i.e.
  // 50 msg/sec). Each transition is two OSC messages (note-off old +
  // note-on new), so the bridge can sustain 25 transitions/sec. We emit
  // at 80 ms per transition (~12 tx/sec = 25 msg/sec) — half the drain
  // rate, so the queue stays near-empty and there's no tail after the
  // user stops moving. Windows setTimeout resolution is ~15 ms, so any
  // value closer to the drain rate causes jitter-driven queue growth.
  // Intermediate keys that fall inside the gap are skipped — the drag
  // becomes a sparser glissando rather than a queued replay.
  const DRAG_MIN_GAP_MS = 80;
  let lastEmitAt = 0;
  let resolveTimer = 0;

  function cancelResolveTimer(): void {
    if (resolveTimer !== 0) {
      clearTimeout(resolveTimer);
      resolveTimer = 0;
    }
  }

  function resolveNow(): void {
    resolveTimer = 0;
    if (activePointerId === null) return;
    const target = noteAtPoint(latestX, latestY);
    if (target === activeKey) return;
    const now = performance.now();
    const since = now - lastEmitAt;
    if (since < DRAG_MIN_GAP_MS) {
      // Too soon — try again when the gap elapses. If the pointer keeps
      // moving before we fire, the next `resolveNow` just reads the
      // newest latestX/Y, so any keys we skip over never become events.
      resolveTimer = window.setTimeout(resolveNow, DRAG_MIN_GAP_MS - since);
      return;
    }
    if (activeKey !== null) releaseNote(activeKey);
    activeKey = target;
    if (target !== null) pressNote(target);
    lastEmitAt = performance.now();
  }

  keyboard.addEventListener('pointerdown', (e) => {
    if (activePointerId !== null) return;
    const note = noteAtPoint(e.clientX, e.clientY);
    if (note === null) return;
    e.preventDefault();
    try { keyboard.setPointerCapture(e.pointerId); } catch { /* ignore */ }
    activePointerId = e.pointerId;
    activeKey = note;
    latestX = e.clientX;
    latestY = e.clientY;
    pressNote(note);
    lastEmitAt = performance.now();
  });

  keyboard.addEventListener('pointermove', (e) => {
    if (activePointerId !== e.pointerId) return;
    latestX = e.clientX;
    latestY = e.clientY;
    if (resolveTimer === 0) resolveNow();
  });

  const endDrag = (e: PointerEvent): void => {
    if (activePointerId !== e.pointerId) return;
    cancelResolveTimer();
    if (activeKey !== null) releaseNote(activeKey);
    activeKey = null;
    activePointerId = null;
    try { keyboard.releasePointerCapture(e.pointerId); } catch { /* already released */ }
  };
  keyboard.addEventListener('pointerup',     endDrag);
  keyboard.addEventListener('pointercancel', endDrag);

  // Prevent the browser from treating a drag across keys as a page
  // pan/scroll on touch devices. Also kills native text selection of
  // key labels when the pointer is mouse-dragged.
  keyboard.style.touchAction = 'none';
  keyboard.style.userSelect  = 'none';

  for (let i = 0; i < whites.length; i++) {
    const n = whites[i];
    const el = document.createElement('div');
    el.className = 'piano-key piano-key-white';
    el.dataset.note = String(n);
    // Label every C for orientation. MIDI 12 = C0, MIDI 60 = C4 (middle C).
    if (n % 12 === 0) {
      const label = document.createElement('span');
      label.className = 'piano-key-label';
      label.textContent = `C${Math.floor(n / 12) - 1}`;
      el.appendChild(label);
    }
    whitesRow.appendChild(el);
    keyByNote.set(n, el);
  }

  // Black keys sit centered on the seam between two white keys. The
  // "preceding" white key for a sharp is one semitone below — C# after C,
  // D# after D, F# after F, and so on — so finding its index in `whites`
  // gives us the seam position directly.
  const blackWidthPct = (100 / whiteCount) * 0.62;
  for (let n = FIRST_NOTE; n <= LAST_NOTE; n++) {
    if (!isBlackKey(n)) continue;
    const precedingIdx = whites.indexOf(n - 1);
    if (precedingIdx < 0) continue;
    const seamPct = ((precedingIdx + 1) / whiteCount) * 100;
    const leftPct = seamPct - blackWidthPct / 2;
    const el = document.createElement('div');
    el.className = 'piano-key piano-key-black';
    el.dataset.note = String(n);
    el.style.left  = `${leftPct}%`;
    el.style.width = `${blackWidthPct}%`;
    blacksLayer.appendChild(el);
    keyByNote.set(n, el);
  }

  keyboard.append(whitesRow, blacksLayer);
  root.append(banner, keyboard);

  let running = false;

  function setNote(note: number, on: boolean, channel?: number): void {
    if (!keyByNote.has(note)) return;
    if (on) echoHeld.add(note);
    else    echoHeld.delete(note);
    syncVisual(note);
    if (channel !== undefined && channel >= 1 && channel <= 16) {
      let set = heldByChannel.get(channel);
      if (on) {
        if (!set) { set = new Set<number>(); heldByChannel.set(channel, set); }
        set.add(note);
      } else if (set) {
        set.delete(note);
        if (set.size === 0) heldByChannel.delete(channel);
      }
      syncChannelReadout();
    }
  }

  function clear(): void {
    pointerHeld.clear();
    echoHeld.clear();
    heldByChannel.clear();
    for (const el of keyByNote.values()) el.classList.remove('active');
    syncChannelReadout();
  }

  function start(): void { running = true; }
  function stop(): void  { running = false; clear(); }
  function isRunning(): boolean { return running; }

  function onPress(cb: (note: number, down: boolean) => void): void {
    pressCb = cb;
  }

  return { element: root, setNote, clear, start, stop, isRunning, onPress };
}
