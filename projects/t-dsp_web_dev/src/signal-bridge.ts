// Signal<T> ↔ Solid signal bridge.
//
// The existing state.ts model uses a hand-rolled Signal<T> class with
// get / set / subscribe semantics — exactly the shape of Solid's
// createSignal. This file lets Solid components consume those signals
// without rewriting state.ts: each component calls asSolid(signal)
// once at the top, and Solid's automatic dependency tracking handles
// the rest.
//
// Why this works without a leak: app-lifetime signals (state.ts
// fields) are never destroyed, so the listener installed by asSolid
// also lives forever — no cleanup required. For component-scoped
// wrappers (rare), use asSolidScoped which registers an onCleanup
// to remove the listener.
//
// See planning/ui-rebuild/04-stack.md for the rationale.

import {
  createSignal as createSolidSignal,
  onCleanup,
  type Accessor,
} from 'solid-js';
import type { Signal as TSignal } from './state';

/**
 * Wrap an existing Signal<T> as a Solid Accessor<T>.
 *
 * The returned getter is reactive — any Solid component or effect
 * that reads it subscribes to the underlying TSignal automatically.
 * Writes still go through the original TSignal.set() path, so the
 * dispatcher and the rest of the app continue to see them.
 *
 * Use for app-lifetime signals (state.ts fields). The installed
 * listener leaks intentionally — these signals never get destroyed.
 */
export function asSolid<T>(s: TSignal<T>): Accessor<T> {
  const [get, set] = createSolidSignal<T>(s.get());
  // The set() function in Solid takes either a value or a function-
  // updater. For T types that happen to be callable (functions), the
  // overload is ambiguous — wrap in a thunk to disambiguate.
  s.subscribe((v) => set(() => v));
  return get;
}

/**
 * Bidirectional helper: returns [get, set] where set() writes to the
 * underlying TSignal. Convenient for two-way bindings (faders, etc).
 *
 * Note: writing via the returned setter triggers the TSignal's own
 * notify path, which calls back into our asSolid listener and writes
 * to the Solid signal. TSignal.set() short-circuits on Object.is so
 * idempotent writes don't fire listeners twice. Net: one effect run
 * per actual change, regardless of which side initiated.
 */
export function asSolidRW<T>(s: TSignal<T>): [Accessor<T>, (v: T) => void] {
  return [asSolid(s), (v: T) => s.set(v)];
}

/**
 * Component-scoped variant — installs the listener under Solid's
 * onCleanup so it's removed when the component unmounts. Use this
 * for signals that exist outside the global state model and might be
 * destroyed before the app exits (e.g. per-voice telemetry that
 * fluctuates with MPE allocation).
 *
 * Idempotent on Object.is, same as asSolid.
 */
export function asSolidScoped<T>(s: TSignal<T>): Accessor<T> {
  const [get, set] = createSolidSignal<T>(s.get());
  const unsub = s.subscribe((v) => set(() => v));
  onCleanup(unsub);
  return get;
}
