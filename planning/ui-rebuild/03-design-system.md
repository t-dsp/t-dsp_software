# 03 — Design system

Touch tokens, control specifications, gesture vocabulary, color tokens,
and Tailwind v4 setup.

## Touch tokens (the base sizes)

These are the **non-negotiable minimums**. Tailwind theme reflects them
as named tokens.

```css
/* Tap target */
--touch-min:       44px;   /* Apple HIG floor */
--touch-default:   56px;   /* Comfortable target */
--touch-large:     72px;   /* Primary action */

/* Body text */
--text-xs:         12px;   /* Secondary metadata only (e.g. dB readout) */
--text-sm:         14px;   /* Default for labels */
--text-base:       16px;   /* Default for content */
--text-lg:         20px;   /* Section headings */
--text-xl:         28px;   /* Workspace tab labels */

/* Spacing (8 px base unit) */
--space-1:          8px;
--space-2:         16px;
--space-3:         24px;
--space-4:         32px;
--space-6:         48px;

/* Control sizing */
--fader-thumb-w:   60px;   /* Mini-bank fader thumb */
--fader-thumb-h:   28px;
--fader-track-w:   12px;
--fader-thumb-w-tune: 80px;  /* TUNE workspace big faders */
--fader-thumb-h-tune: 40px;
--fader-min-h:    240px;    /* TUNE / MIX strip fader */
--fader-min-h-mini: 80px;   /* Bottom-strip mini-fader */

/* Knob (rotary) */
--knob-min:        72px;
--knob-default:    96px;

/* Pad / cell (beats grid, etc.) */
--pad-min:         52px;
--pad-default:     64px;
```

The 9 px label, 11 px button text, and 24 px buttons in current
`style.css` are all **explicit anti-patterns**. None should appear in
new code.

## Color tokens

Dark theme stays — it works at stage, in studio, and on a Surface.
Tokens shift slightly to widen contrast for sunlit operation.

```css
/* Surfaces */
--surface-0:     #0e0e10;   /* deepest, page background */
--surface-1:     #18181b;   /* panel background */
--surface-2:     #27272a;   /* card / strip background */
--surface-3:     #3f3f46;   /* hovered / selected card */
--surface-4:     #52525b;   /* control track / disabled */

/* Foreground */
--fg-1:          #fafafa;   /* primary text */
--fg-2:          #d4d4d8;   /* secondary */
--fg-3:          #a1a1aa;   /* tertiary, captions */
--fg-disabled:   #52525b;

/* Accent (orange — keep current brand-ish) */
--accent-500:    #d68038;
--accent-600:    #b86a30;
--accent-700:    #8d4f23;

/* Status */
--status-ok:     #4ade80;   /* connected, OK */
--status-warn:   #facc15;   /* warning, host bypass active */
--status-error:  #ef4444;   /* error, recording */
--status-info:   #38bdf8;   /* solo / link / cool secondary */

/* Bus colors (for sends-on-faders fader-thumb stripes) */
--bus-main:      #d68038;
--bus-synth:     #a78bfa;
--bus-fx:        #38bdf8;
--bus-aux1:      #34d399;
--bus-aux2:      #f472b6;
--bus-aux3:      #fb923c;
--bus-aux4:      #fbbf24;
```

## Control specifications

### Fader (the most-used control)

- **Track:** 12 px wide, vertically oriented, rounded corners, gradient
  fill from base to current value.
- **Thumb:** 60 × 28 px (mini) / 80 × 40 px (TUNE strip). Pill-shaped,
  high-contrast, with a visible center notch so the user can tell where
  the value is on a static screenshot.
- **Min track height:** 80 px (mini), 240 px (full strip).
- **dB readout:** below the thumb, 12 px tabular-nums monospace, shows
  current value in dB. (`formatFaderDb` from `util.ts` is reused.)
- **Detent at unity (0 dB):** 4 px tick on the track. Two-finger tap
  snaps to it.
- **Value envelope:** custom pointer-event handling, **not** native
  `<input type="range">`. Native range inputs have a thumb-too-small
  problem, no fine mode, and inconsistent rendering across platforms.
- **Fine mode:** two-finger drag scales movement 0.1× for precise
  setting.
- **Throttling:** all drag-driven sets route through `raf-batch.ts`'s
  coalescer. Per [project_serial_bridge_throttle.md](C:/Users/jaysh/.claude/projects/c--github-t-dsp-t-dsp-software/memory/project_serial_bridge_throttle.md),
  TX_GAP_MS=20 ms means UI cannot exceed 50 msg/sec total; ≤25
  msg/sec per fader is the budget.

### Knob (rotary — for engine parameters)

- **Body:** circular, 72-96 px diameter, with a clear position indicator.
- **Range arc:** the unfilled arc (for context) at full saturation
  reduced opacity (~0.3); the filled arc (current value) at full
  saturation.
- **Drag:** **vertical drag only** by default (touch-friendly; circular
  drag is fine on mouse but unreliable on touch). Up = increase, down =
  decrease.
- **Range:** 240 px of vertical drag = full range. Fine mode (two-finger)
  scales 0.1×.
- **Numeric readout:** below or inside the knob; user-controllable
  units (Hz, dB, %, ms, etc.).

### Button / toggle

- **Size:** 56 × 44 px minimum (comfortable touch).
- **States:** off, on, momentary-pressed, disabled. Each state has a
  distinct color/border treatment.
- **For step-grid cells (beats):** 64 × 64 px minimum, with a clear
  on/off visual (filled / outlined).

### Tab (workspace + inner tabs)

- **Workspace tab:** 64 px tall, label at `--text-xl` (28 px), padding
  `--space-3` (24 px) horizontal. Active tab has full-saturation accent
  underline.
- **Inner tab:** 48 px tall, label at `--text-base` (16 px), padding
  `--space-2` (16 px) horizontal.

### Pad / cell (beats step, looper transport buttons)

- **Min:** 52 × 52 px. Default 64 × 64 px.
- **Step grid:** rows 64 px tall, with column gaps. 16 steps × 64 px ≈
  1024 px wide — fits comfortably on a Surface.

## Gesture implementation

Use **Pointer Events** (no library). Browser support is universal in
Chromium-based hosts. Track multi-touch via `pointerId`.

### Long-press

```ts
let pressTimer: number | undefined;
el.addEventListener('pointerdown', e => {
  pressTimer = window.setTimeout(() => onLongPress(e), 400);
});
el.addEventListener('pointerup',     () => clearTimeout(pressTimer));
el.addEventListener('pointercancel', () => clearTimeout(pressTimer));
el.addEventListener('pointermove', e => {
  if (movedFar(e)) clearTimeout(pressTimer);  // cancel on drift
});
```

### Swipe on tab bar / fader bank

Threshold: 80 px horizontal travel within 400 ms, vertical drift < 30 px.

### Two-finger gestures

Track active pointers in a `Map<pointerId, PointerState>`. On the second
`pointerdown`, switch the active drag to fine mode. On any
`pointercancel` or `pointerup`, return to single-finger mode.

### Pull-down / pull-up drawer

Treat as a dedicated drag handle; vertical drag past 50 % triggers
expand/collapse on release, < 50 % springs back. Use CSS transform on
the drawer element so animation is GPU-driven.

## Tailwind v4 setup

### Why v4 specifically

- **`@theme` block** lets us declare design tokens directly in CSS in
  one place, no JavaScript config file. Tokens become CSS custom
  properties automatically.
- **No `tailwind.config.js`** — just CSS. Less plumbing.
- **Better tree-shaking** of utility classes than v3.

### `tailwind.css` (theme + base layer)

```css
@import "tailwindcss";

@theme {
  --color-surface-0: #0e0e10;
  --color-surface-1: #18181b;
  --color-surface-2: #27272a;
  --color-surface-3: #3f3f46;
  --color-surface-4: #52525b;

  --color-fg-1: #fafafa;
  --color-fg-2: #d4d4d8;
  --color-fg-3: #a1a1aa;
  --color-fg-disabled: #52525b;

  --color-accent-500: #d68038;
  --color-accent-600: #b86a30;
  --color-accent-700: #8d4f23;

  --color-bus-main:  #d68038;
  --color-bus-synth: #a78bfa;
  --color-bus-fx:    #38bdf8;

  --spacing-touch-min: 44px;
  --spacing-touch-default: 56px;
  --spacing-touch-large: 72px;

  --text-xl: 28px;
  --text-base: 16px;
}

/* Custom layer for things Tailwind utilities can't easily express. */
@layer components {
  .fader-track {
    width: 12px;
    background: linear-gradient(to top,
                                var(--color-accent-700),
                                var(--color-accent-500));
    border-radius: 6px;
  }
  /* etc. */
}
```

### Vite plugin

```ts
// vite.config.ts
import { defineConfig } from 'vite';
import tailwindcss from '@tailwindcss/vite';
import solid from 'vite-plugin-solid';

export default defineConfig({
  plugins: [tailwindcss(), solid()],
});
```

## Layout breakpoints

| Width | Target |
|---|---|
| < 1280 px | "Compact": fader bank shows 4 channels; bottom strip slim |
| 1280-1920 px | **Default**: Surface Pro / Surface Studio touchscreens |
| > 1920 px | "Wide": fader bank shows all 10 channels at once; TUNE uses extra width for bigger EQ canvas |

These are width breakpoints expressed via Tailwind's `sm`/`md`/`lg`/`xl`
overrides. There is **no mobile / phone breakpoint** — phones aren't a
target.

## Animation budget

- **Transitions on hover/active states:** 100 ms, ease-out.
- **Drawer expand/collapse:** 200 ms, cubic-bezier(0.2, 0, 0, 1).
- **Workspace switch (no animation in v1):** instant. Animation is
  optional polish for later.
- **Sel highlight pulse:** 200 ms scale + opacity flash on Sel change.
- **Fader / meter:** **no transitions ever**. Direct transform / scaleY
  driven by the underlying signal. Per [01-current-state.md](01-current-state.md)
  performance baselines.

## Accessibility (deferred but not anti)

Not in this epic, but **don't actively break it**:

- Use semantic `<button>` for buttons, `<nav>` for tab bars, etc.
- Maintain focus outlines on keyboard navigation.
- ARIA roles where straightforward (`role="tab"`, `aria-selected`).
- Enough contrast in dark theme (the proposed palette already exceeds
  WCAG AA for body text).

A full a11y audit is a future epic. For now, "no regressions versus
current" is the bar.
