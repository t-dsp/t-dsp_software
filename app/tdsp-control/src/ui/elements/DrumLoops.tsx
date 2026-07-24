// DrumLoops.tsx — the Drums track's "Drum Loops" player, pulled out of App.tsx so the whole
// drum-groove UI lives in one place. Two presentational pieces, both fed by the drum song deck:
//   • DrumLoopsActions — the card/page header transport (‹ prev / next › / ▶ play / ■ stop).
//   • DrumLoopsBody     — the detail page: the groove Volume slider + the /midi/drums folder browser.
// A drum groove always loops, so (unlike the synth song players) there's no end-mode control here —
// which is exactly why this specialises the shared song renderers into their own drum-only file.
// Purely presentational: App owns the deck state + wiring; this just renders it.
import React from 'react';
import { View, Text } from 'react-native';
import { HdrBtn, VolSlider, FolderBrowser } from '../primitives';
import { s } from '../styles';
import type { Transport } from '../../transport';

// The slice of App's SongDeckT that the drum loops UI actually reads. App passes its `drumDeck`
// straight through — this narrower shape keeps the component decoupled from App's internal type.
export type DrumDeck = {
  player: { song: string; playing: boolean };
  vol: number; onVol: (n: number) => void; commitVol: (n: number) => void; volNote?: string;
  playFile: (arg: string, disp: string) => void;
  play: () => void; stop: () => void; step: (dir: number) => void;
  onFolderList: (files: { arg: string; name: string }[]) => void;
  browseRoot?: string; injectFolders?: { name: string; leaves: { name: string; arg: string }[] }[];
  filesFirst?: boolean;
};

// Header transport shown on both the Drum Loops card (tile) and its page. ‹/› step through the
// current groove folder; ▶ restarts the selected groove on a fresh downbeat; ■ stops the drums.
// Each button is capped at 50px (s.hdrBtnCap) — the same compact Play Control style the synth
// landing cards use — so the row stays tight instead of stretching each button.
export function DrumLoopsActions({ deck }: { deck: DrumDeck }) {
  return (
    <>
      <View style={{ flex: 1 }} />{/* spacer: push the capped controls to the right edge of the row */}
      <HdrBtn label="‹" stop onPress={() => deck.step(-1)} style={s.hdrBtnCap} />
      <HdrBtn label="›" stop onPress={() => deck.step(1)} style={s.hdrBtnCap} />
      <HdrBtn label="▶" onPress={deck.play} active={deck.player.playing} style={s.hdrBtnCap} />
      <HdrBtn label="■" stop onPress={deck.stop} style={s.hdrBtnCap} />
    </>
  );
}

// The Drum Loops detail page: groove Volume, then the recursive /midi/drums folder browser (with the
// injected ★ Featured shortcut). A tap plays the groove immediately (restart on a fresh downbeat).
export function DrumLoopsBody({ deck, tp, connected, loaded }: { deck: DrumDeck; tp: Transport; connected: boolean; loaded: boolean }) {
  return (
    <>
      <VolSlider label="Volume" value={deck.vol} onChange={deck.onVol} onCommit={deck.commitVol} disabled={!connected} />
      {!!deck.volNote && <Text style={s.muted}>{deck.volNote}</Text>}
      <View style={s.browseBox}>
        <FolderBrowser tp={tp} root={deck.browseRoot ?? '/midi/drums'} ext="mid" enabled={connected && loaded}
          selected={deck.player.song} playing={deck.player.playing ? deck.player.song : undefined}
          onSelectFile={(full, disp) => deck.playFile(full, disp)} injectFolders={deck.injectFolders}
          onFolderList={deck.onFolderList} filesFirst={deck.filesFirst} />
      </View>
    </>
  );
}
