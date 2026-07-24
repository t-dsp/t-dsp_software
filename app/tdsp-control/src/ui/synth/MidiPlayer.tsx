// MidiPlayer.tsx — the MIDI Player feature for a synth voice, CARD + DETAIL PAGE in one file.
// (The user plans to rename this "Looper"; keeping the MIDI-Player names for now.)
//   • playerValue(deck) / playerProgress(deck) → the card's value line + progress bar
//   • playerActions(deck)                       → the card's transport row (‹ › ▶ ■ + end-mode)
//   • playerSongBody(deck, ctx)                 → the song selector (Volume, folder browser, end-mode)
//   • playerBody(deck, ctx)                     → the detail page: MIDI PLAYER / MIDI LOOPER / NOTE
//                                                 EDITOR tabs (looper + editor only on a recorder build)
// The deck (SongDeck) carries all per-voice state + handlers; `ctx` carries the shared App bits the
// body needs (transport, connection/load flags, recorder caps, the looper-tab renderer). Extracted
// verbatim from App.tsx; App builds `ctx` once and passes it through.
import React from 'react';
import { View, Text, Pressable } from 'react-native';
import { s } from '../styles';
import { HdrBtn, Row, VolSlider, FolderBrowser, BodyTabs } from '../primitives';
import PianoRoll from '../PianoRoll';
import { END_MODES, InjectFolder } from '../constants';
import type { Transport } from '../../transport';
import type { SongDeck } from './types';

// Shared App context the player detail body needs beyond the deck itself.
export type PlayerCtx = {
  tp: Transport; connected: boolean; loaded: boolean;
  songInjectFolders: InjectFolder[];   // baked "built-ins" demos injected at the /midi root
  hasRec: boolean;                     // caps.rec — show the MIDI LOOPER tab
  hasRecEdit: boolean;                 // caps.recedit — show the NOTE EDITOR tab
  recRow: (v: number) => React.ReactNode;   // the MIDI LOOPER tab content (stays in App; uses rec state)
  recMax: number;                      // rec.max for the note editor
};

// A quick loop-record toggle for the transport row: `active` = the looper is armed/capturing (button
// glows red), `onToggle` arms a fresh recording when idle and closes/stops it when active. Omitted on
// builds without the recorder (caps.rec) — the ● button then simply doesn't render.
export type RecToggle = { active: boolean; onToggle: () => void };

export const playerActions = (D: SongDeck, rec?: RecToggle) => (<>
  <HdrBtn label="‹" stop onPress={() => D.step(-1)} style={s.hdrBtnCap} />
  <HdrBtn label="›" stop onPress={() => D.step(1)} style={s.hdrBtnCap} />
  <HdrBtn label="▶" onPress={D.play} active={D.player.playing} style={s.hdrBtnCap} />
  <HdrBtn label="■" stop onPress={D.stop} style={s.hdrBtnCap} />
  {rec && <HdrBtn label="●" stop={!rec.active} onPress={rec.onToggle} style={[s.hdrBtnCap, rec.active && s.hdrBtnRec]} />}
  {!D.noEndMode && <HdrBtn label={(END_MODES.find(m => m.key === D.endMode) || END_MODES[3]).icon} stop onPress={D.cycleEnd} style={s.hdrBtnCap} />}
</>);

// The song half: pick a song via the recursive folder browser, set the player's level, choose
// what happens when it ends. A tap plays the file immediately (restart on a fresh downbeat).
export const playerSongBody = (D: SongDeck, ctx: PlayerCtx) => (
  <>
    <VolSlider label="Volume" value={D.vol} onChange={D.onVol} onCommit={D.commitVol} disabled={!ctx.connected} />
    {!!D.volNote && <Text style={s.muted}>{D.volNote}</Text>}
    <View style={s.browseBox}>
      <FolderBrowser tp={ctx.tp} root={D.browseRoot ?? '/midi'} ext="mid" enabled={ctx.connected && ctx.loaded}
        selected={D.player.song} playing={D.player.playing ? D.player.song : undefined}
        onSelectFile={(full, disp) => D.playFile(full, disp)} injectFolders={D.injectFolders ?? ctx.songInjectFolders}
        onFolderList={D.onFolderList} filesFirst={D.filesFirst} />
    </View>
    {!D.noEndMode && <Row><Text style={[s.muted, { flex: 1 }]}>When finished</Text>
      {END_MODES.map(m => (
        <Pressable key={m.key} style={[s.pill, D.endMode === m.key && s.pillOn]} onPress={() => D.applyEnd(m.key)}>
          <Text style={s.text}>{m.icon}  {m.label}</Text>
        </Pressable>
      ))}</Row>}
  </>
);

// The player page splits in two (three on a recorder build): the song selector, THIS synth's own
// loop recorder, and the note editor. The header transport (‹ › ▶ ■) drives the SONG on all tabs.
export const playerBody = (D: SongDeck, ctx: PlayerCtx) => (
  <BodyTabs tabs={[
    { key: 'song', label: 'MIDI PLAYER', body: playerSongBody(D, ctx) },
    ...(ctx.hasRec ? [{ key: 'loop', label: 'MIDI LOOPER', body: ctx.recRow(D.v) }] : []),
    ...(ctx.hasRec && ctx.hasRecEdit ? [{ key: 'edit', label: 'NOTE EDITOR', body: <PianoRoll tp={ctx.tp} voice={D.v} maxEvents={ctx.recMax} /> }] : []),
  ]} />
);

// Card/page subtitle + progress bar (the bar only once the device reports a position).
export const playerValue = (D: SongDeck) => (D.player.playing ? '♪ ' : '') + (D.player.name || '—');
export const playerProgress = (D: SongDeck) => (D.player.playing && D.player.prog >= 0 ? D.player.prog : undefined);
