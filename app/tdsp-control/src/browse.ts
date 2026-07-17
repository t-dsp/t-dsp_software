// browse.ts — shared parser for the generic @LS directory-list reply.
//
// Lives in its OWN module (NOT transport.ts) for the same reason as dxls.ts: transport.ts
// has platform siblings transport.web.ts / transport.native.ts, so a runtime `import … from
// './transport'` inside a platform file resolves back to that SAME platform file (the
// .web/.native extension wins), yielding `undefined` at runtime. A neutral filename with no
// platform variants resolves unambiguously from every importer. Type-only imports are erased,
// so importing types from './transport' stays safe.
//
// Wire framing (mirrors @READ's @FB/@FD/@FE), one small line per entry:
//   @LB=<id>\x1f<path>            begin (id = device counter)
//   @LD=<id>\x1f<D|F>\x1f<name>   one entry (bare name — D=subdir, F=file)
//   @LE=<id>\x1f<count>           end (number of @LD lines)
//   @LERR=<id>\x1f<reason>        error
// The transport accumulates the frames (keyed by id, stale-rejected by echoed path) and
// resolves a BrowseResult. These helpers just parse the individual line bodies (the text
// AFTER the "@Lx=" prefix), keeping the transport free of parsing logic.

export type BrowseType = 'D' | 'F';
export interface BrowseEntry { type: BrowseType; name: string; }
export interface BrowseResult { path: string; entries: BrowseEntry[]; }

const FS = '\x1f';

// "@LB=" body -> { id, path }.
export function parseLb(body: string): { id: number; path: string } {
  const p = body.split(FS);
  return { id: +p[0] || 0, path: p[1] ?? '' };
}

// "@LD=" body -> { id, type, name }, or null if the type field isn't D/F.
export function parseLd(body: string): { id: number; type: BrowseType; name: string } | null {
  const p = body.split(FS);
  const t = p[1];
  if (t !== 'D' && t !== 'F') return null;
  return { id: +p[0] || 0, type: t, name: p[2] ?? '' };
}

// "@LE=" body -> { id, count }.
export function parseLe(body: string): { id: number; count: number } {
  const p = body.split(FS);
  return { id: +p[0] || 0, count: +p[1] || 0 };
}

// "@LERR=" body -> { id, reason }.
export function parseLerr(body: string): { id: number; reason: string } {
  const p = body.split(FS);
  return { id: +p[0] || 0, reason: p[1] ?? 'error' };
}

// Sort a listing for display: folders first, then files, each case-insensitively by name.
// The firmware streams in raw filesystem order — the CLIENT owns sorting.
export function sortEntries(entries: BrowseEntry[]): BrowseEntry[] {
  return entries.slice().sort((a, b) =>
    a.type !== b.type ? (a.type === 'D' ? -1 : 1)
      : a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
}
