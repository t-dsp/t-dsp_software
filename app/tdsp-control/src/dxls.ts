// dxls.ts — shared @DXLS reply parser for the lazy /dexed browser.
//
// This lives in its OWN module (not transport.ts) on purpose: transport.ts has
// platform siblings transport.web.ts / transport.native.ts, so Metro resolves an
// `import … from './transport'` inside a platform file back to that SAME platform
// file (the .web/.native extension wins), not to transport.ts. Type-only imports are
// erased so they're unaffected, but a runtime VALUE like parseDxls would resolve to
// `undefined` (the "parseDxls is not a function" crash). A neutral filename with no
// platform variants resolves unambiguously from every importer.

import type { DirPage } from './transport';

// Parse a @DXLS reply body: "<rel>\t<page>\t<npages>|D<dir>|F<cart>..." -> DirPage.
// Cart `rel` is built from the echoed folder path + the raw filename (with .syx, which
// @DXVL/@DXPICK need); `name` strips .syx for display.
export function parseDxls(payload: string): DirPage {
  const bar = payload.indexOf('|');
  const head = (bar >= 0 ? payload.slice(0, bar) : payload).split('\t');
  const path = head[0] || '';
  const page = +head[1] || 0;
  const npages = +head[2] || 1;
  const folders: string[] = [];
  const carts: { name: string; rel: string }[] = [];
  const items = bar >= 0 ? payload.slice(bar + 1).split('|').filter(s => s.length) : [];
  for (const s of items) {
    const nm = s.slice(1);
    if (s[0] === 'D') folders.push(nm);
    else carts.push({ name: nm.replace(/\.syx$/i, ''), rel: path ? path + '/' + nm : nm });
  }
  return { path, page, npages, folders, carts };
}
