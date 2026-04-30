// Client-side gang state for the DAC EQ tab. When enabled, edits to
// Output 1's biquad bands are mirrored to Output 2's matching band over
// OSC, and the Output 2 section's controls are visually disabled.
//
// Lives outside biquad-widget.ts so the codec panel renderer (which owns
// the toggle control) and the band widgets (which read the gang state on
// every push) share one signal. No firmware participation — the gang is
// purely how the UI fans out writes.

import { Signal } from './state';

export const eqLinkSignal = new Signal<boolean>(false);
