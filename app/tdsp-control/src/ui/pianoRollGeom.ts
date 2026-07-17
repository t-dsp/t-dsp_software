// pianoRollGeom.ts — pixel<->grid geometry + the drag state machine for a 2-D roll editor.
//
// MIDI-FREE BY CONTRACT (planning/midi-editor/DESIGN.md §6.4). This layer knows only rows,
// columns, item rectangles and an opaque id — never pitch, ticks, channels, or transports.
// That is exactly what lets the phase-3 arp roll reuse the whole interaction layer with a
// different row axis. THE CANARY: do not `import ... from './loopClip'` here. If that import
// ever appears, a MIDI assumption has leaked into the layer meant to be reused.

// A rectangle on the grid. `start`/`length` are in COLUMN units (the roll's x-unit, e.g. ticks);
// `lane` is a 0-based ROW index from the TOP. `id` is opaque (the model owns its meaning).
export type GridItem = { id: string; start: number; length: number; lane: number };

// Pixels per column-unit / per row, and the totals. The single source of truth for layout.
export type Viewport = { colW: number; rowH: number; cols: number; rows: number };

export const gx = (col: number, vp: Viewport) => col * vp.colW;
export const gy = (lane: number, vp: Viewport) => lane * vp.rowH;
export const toCol = (x: number, vp: Viewport) => x / vp.colW;
export const toLane = (y: number, vp: Viewport) => Math.floor(y / vp.rowH);

export const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);
export const snap = (col: number, grid: number) => Math.round(col / grid) * grid;

// Topmost item containing (x,y), and whether the touch is near its right edge (a resize grab).
// `edgePx` widens both the horizontal hit margin and the resize zone for fingers.
export function hitTest(
  items: GridItem[], x: number, y: number, vp: Viewport, edgePx: number,
): { item: GridItem; onRightEdge: boolean } | null {
  for (let i = items.length - 1; i >= 0; i--) {
    const it = items[i];
    const ix = it.start * vp.colW, iw = it.length * vp.colW, iy = it.lane * vp.rowH;
    if (x >= ix - edgePx && x <= ix + iw + edgePx && y >= iy && y <= iy + vp.rowH) {
      return { item: it, onRightEdge: x >= ix + iw - edgePx };
    }
  }
  return null;
}

export type DragMode = 'none' | 'move' | 'resize' | 'create';

export type DragState = {
  mode: DragMode;
  id: string | null;                                   // the item being changed / created
  baseStart: number; baseLength: number; baseLane: number;   // item geometry at grab time
  committed: boolean;                                  // has the touch passed the dead zone yet
};

export const DEAD_PX = 8;   // finger-wobble threshold: below this a drag is still a tap

// Apply a drag delta (in COLUMN units and LANE steps) to the grabbed item's geometry.
// Pure — snapping and clamping only, no knowledge of what a lane/column means.
export function applyDrag(
  d: DragState, dCol: number, dLane: number, grid: number, vp: Viewport,
): { start: number; length: number; lane: number } {
  let start = d.baseStart, length = d.baseLength, lane = d.baseLane;
  if (d.mode === 'move') {
    start = clamp(snap(d.baseStart + dCol, grid), 0, Math.max(0, vp.cols - grid));
    lane  = clamp(d.baseLane + dLane, 0, vp.rows - 1);
  } else if (d.mode === 'resize' || d.mode === 'create') {
    length = Math.max(grid, snap(d.baseLength + dCol, grid));   // never shorter than one grid unit
  }
  return { start, length, lane };
}
