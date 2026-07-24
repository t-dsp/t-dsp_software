// SynthCard.tsx — the synthesizer LANDING card, styled like the Drums card: a square album-cover media
// box (instrument glyph + the synth's letter) on the left, a "SYNTHESIZER x" kicker, the loaded PRESET
// as the big hero title, the MEDIA (song) as the second hero line, and Engine / Arp / FX flag chips —
// with a playback bar + Volume foot and the play controls (‹Ins / Ins› / ▶ / ■ / Arp / ↻). Purely
// presentational: App supplies all the data + handlers; this renders them through the shared <Card>.
// Used both as a home-grid tile (onPress opens the section) and as the folded first tile of its own
// child grid on the detail page — neither shows a chevron/back arrow (nav is via the menu bar).
import React from 'react';
import { View, Text, Pressable, useWindowDimensions } from 'react-native';
import { Card, Flag, VolSlider, HdrBtn, KbdBtn } from '../primitives';
import { s } from '../styles';

export type SynthCardProps = {
  accent?: string; tint?: string;
  letter: string;                // "A" — shown in the media box + kicker
  preset: string;                // hero title: the loaded voice (e.g. "RitChie 1:FM-Rhodes") or "No preset"
  path?: string;                 // small breadcrumb under the title: the voice's /dexed folder path (e.g. "Voices › DX7_AllTheWeb › Big Mamma › 100 through 360"); omit → no path line
  media: string;                 // second hero line: the loaded/playing song (e.g. "♪ 02 Midi Test Chord")
  engine: string;                // Engine flag value (Dexed / OPLL / Plaits …)
  arp: string;                   // Arp flag value ("Simple Up · 1/16" / "Off")
  fx?: string;                   // FX flag value ("Spring 0%"); omit → no FX chip (build without reverb)
  progress?: number;             // playback position (0..1) of whatever's playing (song or loop) — bar + 0-100% above the volume
  connected: boolean;            // disables the controls when not connected
  // USB-keyboard claim glyph (top-right): white = this synth hears the keyboard, grey = it doesn't.
  kbdOwned: boolean; onClaimKbd: () => void;
  onOpenVoices?: () => void;      // top-right › button → open this synth's Synth/Voices browser page
  // Volume foot slider (0..150 %, the per-track @TRK<i>.VOL) + a mute toggle to its left (0% ⇄ restore).
  vol: number; onVol: (n: number) => void; onVolCommit: (n: number) => void; onToggleMute: () => void;
  // Play controls.
  playing: boolean;
  onInsPrev: () => void; onInsNext: () => void;   // ‹ Ins / Ins › — step the instrument/preset
  onPlay: () => void; onStop: () => void;
  arpOn?: boolean; onToggleArp?: () => void;      // Arp button (omit onToggleArp to hide it)
  repeatOn: boolean; onToggleRepeat: () => void;  // ↻ — loop the current song
  // Rendered as a home-grid tile (onPress) OR the folded lead of its detail grid (gridLead). No back
  // arrow either way — onBack is accepted but not forwarded, so the card never shows a chevron.
  onPress?: () => void; onBack?: () => void; gridLead?: boolean;
};

export function SynthCard(p: SynthCardProps) {
  const { width } = useWindowDimensions();
  const narrow = width < 640;   // mobile / narrow window: smaller media/text + compact controls
  // Engine / Arp / FX state chips — bottom-left of the card, not up under the title, so the hero
  // area stays clean.
  const flags = (
    <View style={[s.flagRow, s.synthFlagRow]}>
      <Flag label="Engine" value={p.engine} />
      <Flag label="Arp" value={p.arp} />
      {p.fx && <Flag label="FX" value={p.fx} />}
    </View>
  );
  // Play controls (cap): 50px on mobile, 1.5× on desktop — the sizing is handled inside HdrBtn.
  const controls = (
    <>
      <HdrBtn label="‹ Ins" stop onPress={p.onInsPrev} cap />
      <HdrBtn label="Ins ›" stop onPress={p.onInsNext} cap />
      <HdrBtn label="▶" onPress={p.onPlay} active={p.playing} cap />
      <HdrBtn label="■" stop onPress={p.onStop} cap />
      {p.onToggleArp && <HdrBtn label="Arp" onPress={p.onToggleArp} active={!!p.arpOn} cap />}
      <HdrBtn label="↻" active={p.repeatOn} onPress={p.onToggleRepeat} cap />
    </>
  );
  // Flags ALWAYS sit on their own row ABOVE the play controls — never beside them (that's what squeezed
  // them into a narrow multi-line column). With the full card width the Engine/Arp/FX chips fit on one line.
  const actions = (
    <View style={s.synthActionsStack}>
      {flags}
      <View style={s.synthControlsRow}>{controls}</View>
    </View>
  );
  // Album-cover box: an instrument glyph over the synth's letter, tinted to the track accent. 2× on
  // wide, stepped down to ~1.4× on mobile so it doesn't dominate the tile.
  const leading = (
    <View style={narrow ? s.synthMediaSm : s.synthMedia}>
      <Text style={narrow ? s.synthMediaEmojiSm : s.synthMediaEmoji}>🎹</Text>
      <Text style={[narrow ? s.synthMediaLetterSm : s.synthMediaLetter, p.accent && { color: p.accent }]}>{p.letter}</Text>
    </View>
  );
  // Playback bar for whatever's playing (song OR loop), 0-100%, sitting just above the volume slider.
  // Aligned to the volume row (52px label / 44px value) so the two tracks line up.
  const pct = Math.round(Math.max(0, Math.min(1, p.progress ?? 0)) * 100);
  const foot = (
    <>
      <View style={s.volRow}>
        <Text style={[s.muted, { width: 52 }]}>Playing</Text>
        <View style={s.progTrackFoot}><View style={[s.progFill, { width: `${pct}%` }]} /></View>
        <Text style={[s.muted, { width: 44, textAlign: 'right' }]}>{pct}%</Text>
      </View>
      <View style={s.volMuteRow}>
        <Pressable onPress={p.onToggleMute} disabled={!p.connected} style={[s.muteBtn, p.vol === 0 && s.muteBtnOn]}
          accessibilityLabel={p.vol === 0 ? 'Unmute this synth' : 'Mute this synth'}>
          <Text style={s.muteTxt}>{p.vol === 0 ? '🔇' : '🔊'}</Text>
        </Pressable>
        <View style={{ flex: 1 }}>
          <VolSlider label="Vol" value={p.vol} onChange={p.onVol} onCommit={p.onVolCommit} disabled={!p.connected} />
        </View>
      </View>
    </>
  );
  // The hero area under the title: a small /dexed folder breadcrumb (when the voice lives in the library)
  // then the media/song line (2× — the currently-playing item). State chips moved to the bottom-left.
  const subtitle = (
    <>
      {!!p.path && <Text style={s.synthPathLine} numberOfLines={1} ellipsizeMode="head">{p.path}</Text>}
      <Text style={[s.heroSub, narrow ? s.synthHeroSubSm : s.synthHeroSub]} numberOfLines={1}>{p.media}</Text>
    </>
  );
  // Top-right: the USB-keyboard claim glyph + a › button that opens this synth's Synth/Voices browser
  // (that card was folded into here, so its open affordance lives on the Synthesizer card now).
  const topRight = (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 6 }}>
      <KbdBtn owned={p.kbdOwned} onPress={p.onClaimKbd} />
      {p.onOpenVoices && <HdrBtn label="›" onPress={p.onOpenVoices} cap style={[s.hdrBtnChip, { width: narrow ? 50 : 75 }]} />}
    </View>
  );
  return (
    <Card accent={p.accent} tint={p.tint}
      leading={leading} kicker={'Synthesizer ' + p.letter} title={p.preset || 'No preset'} titleStyle={narrow ? s.synthHeroTitleSm : s.synthHeroTitle} subtitle={subtitle}
      actions={actions} foot={foot}
      topRight={topRight}
      onPress={p.onPress} stackTop
      style={p.onPress || p.gridLead ? s.cardGridAuto : undefined} />
  );
}
