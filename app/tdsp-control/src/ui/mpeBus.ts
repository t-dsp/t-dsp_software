// mpeBus.ts — module-scope pub/sub for the live MPE input/chain trace (@MPE= lines).
//
// The firmware's @MPEMON=1 trace streams one line per incoming controller MIDI event
// AND per post-arp event the synth receives. During a pitch-bend drag that is easily
// ~100 events/sec. Driving App-level React state at that rate would re-render the whole
// app and starve the Web Serial reader loop (the exact hazard the catalog ProgressBus
// was built to avoid — see App.tsx). So @MPE lines flow through THIS bus straight to the
// <MpeMonitor> component, which owns its own refs and repaints at frame rate in isolation.

// One parsed trace event.
//   dir : 'u' USB-host | 'd' DIN | 'b' BT | 's' serial-in   (any of these = INPUT from a device)
//         'o' = OUTPUT — post router+arp, exactly what the synth sink received.
//   ev  : 'n' note-on | 'x' note-off | 'b' pitch-bend | 'p' pressure(Z) | 'c' control-change
//   ch  : 1..16 MIDI channel (MPE: 1 = master, 2..16 = per-note member channels)
//   v1,v2 : INPUT  = raw MIDI (bend v1 = -8192..8191; press/cc value 0..127; note v1 / vel v2)
//           OUTPUT = normalized (bend v1 = centi-semitones; press/timbre v1 = 0..1000 permille)
//   t   : firmware millis() timestamp at emit (device clock)
//   at  : client Date.now() at arrival (for age/fade in the UI)
export interface MpeEvent {
  dir: 'u' | 'd' | 'b' | 's' | 'o' | '?';
  ev: 'n' | 'x' | 'b' | 'p' | 'c' | '?';
  ch: number;
  v1: number;
  v2: number;
  t: number;
  at: number;
}

export const MPE_IN_DIRS = ['u', 'd', 'b', 's'] as const;
export function isInput(dir: string): boolean { return dir === 'u' || dir === 'd' || dir === 'b' || dir === 's'; }

// Parse one "@MPE=u,n,2,60,100,12345" line. Returns null if it is not a well-formed trace
// line (so the caller can cheaply ignore the @MPEMON= ack and any other @M… traffic).
export function parseMpeLine(line: string): MpeEvent | null {
  if (!line.startsWith('@MPE=')) return null;
  const p = line.slice(5).split(',');
  if (p.length < 5) return null;
  const ch = parseInt(p[2], 10), v1 = parseInt(p[3], 10), v2 = parseInt(p[4], 10);
  const t = p.length > 5 ? parseInt(p[5], 10) : 0;
  if (isNaN(ch) || isNaN(v1) || isNaN(v2)) return null;
  return { dir: p[0] as MpeEvent['dir'], ev: p[1] as MpeEvent['ev'], ch, v1, v2, t: isNaN(t) ? 0 : t, at: Date.now() };
}

class MpeBus {
  private listeners = new Set<(ev: MpeEvent) => void>();
  emit = (ev: MpeEvent) => { this.listeners.forEach(l => l(ev)); };
  subscribe(l: (ev: MpeEvent) => void) { this.listeners.add(l); return () => { this.listeners.delete(l); }; }
}

// Singleton — App.tsx's onLine handler pushes into it; <MpeMonitor> subscribes.
export const mpeBus = new MpeBus();
