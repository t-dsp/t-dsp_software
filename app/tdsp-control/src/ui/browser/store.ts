// store.ts — the favorites + recents persistence behind <MediaBrowser>. Each browser instance
// namespaces its own two lists by a `scope` string (e.g. 'drums', 'songs', 'voices'), so the
// drum browser's stars never bleed into the song browser's. Backed by AsyncStorage (→ localStorage
// on web) via the SAME pattern the catalog cache uses (see constants.ts) — no new dependency. This
// module has no platform siblings, so its plain './store' import resolves unambiguously everywhere.
import { useCallback, useEffect, useRef, useState } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';

// One remembered item: the play ARG (full SD path, or a baked leaf's own arg) + a display name.
// arg is the identity — favorites/recents dedupe on it, and it round-trips straight back into the
// deck's playFile()/onSelectFile the same way a live browser tap does. `onPress` is an OPTIONAL
// bespoke tap action used by SOURCE-mode browsers (voices) whose items don't play by path; it is
// never persisted (JSON.stringify drops functions), so favorites/recents stay pure {arg,name}.
export type BrowserItem = { arg: string; name: string; onPress?: () => void };

const RECENTS_MAX = 24;   // cap the recents ring so it stays a quick-access shelf, not a history log (v1)
const favKey = (scope: string) => 'tdsp.browser.fav.' + scope + '.v1';
const recKey = (scope: string) => 'tdsp.browser.recent.' + scope + '.v1';

const load = async (key: string): Promise<BrowserItem[]> => {
  try { const s = await AsyncStorage.getItem(key); const v = s ? JSON.parse(s) : null; return Array.isArray(v) ? v : []; }
  catch { return []; }
};
const save = (key: string, v: BrowserItem[]) => { AsyncStorage.setItem(key, JSON.stringify(v)).catch(() => {}); };

// The browser's Favorites (★-toggled, order preserved) and Recents (most-recent-first, deduped by
// arg, capped) for one `scope`. Loads once per scope; every mutation writes through to AsyncStorage.
// Returns stable callbacks so <MediaBrowser> can list them, star/unstar rows, and log plays.
export function useBrowserLists(scope: string) {
  const [favs, setFavs] = useState<BrowserItem[]>([]);
  const [recents, setRecents] = useState<BrowserItem[]>([]);
  // Guard against a slow initial load clobbering an early write: ignore the loaded value once the
  // user has already touched the list this session.
  const touched = useRef(false);

  useEffect(() => {
    let alive = true;
    touched.current = false;
    setFavs([]); setRecents([]);
    Promise.all([load(favKey(scope)), load(recKey(scope))]).then(([f, r]) => {
      if (alive && !touched.current) { setFavs(f); setRecents(r); }
    });
    return () => { alive = false; };
  }, [scope]);

  const isFav = useCallback((arg: string) => favs.some(f => f.arg === arg), [favs]);

  const toggleFav = useCallback((item: BrowserItem) => {
    touched.current = true;
    setFavs(prev => {
      const next = prev.some(f => f.arg === item.arg)
        ? prev.filter(f => f.arg !== item.arg)
        : [...prev, item];
      save(favKey(scope), next);
      return next;
    });
  }, [scope]);

  // Record a play at the front of the recents ring (deduped, capped). Called on every tap-to-play.
  const addRecent = useCallback((item: BrowserItem) => {
    touched.current = true;
    setRecents(prev => {
      const next = [item, ...prev.filter(r => r.arg !== item.arg)].slice(0, RECENTS_MAX);
      save(recKey(scope), next);
      return next;
    });
  }, [scope]);

  return { favs, recents, isFav, toggleFav, addRecent };
}
