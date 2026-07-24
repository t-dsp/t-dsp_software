// SynthCard.tsx — the synthesizer LANDING card, styled like the Drums card: a square album-cover media
// box (instrument glyph + the synth's letter) on the left, a "SYNTHESIZER x" kicker, the loaded PRESET
// as the big hero title, the MEDIA (song) as the second hero line, and Engine / Arp / FX flag chips —
// with a playback bar + Volume foot and the play controls (‹Ins / Ins› / ▶ / ■ / Arp / ↻). Purely
// presentational: App supplies all the data + handlers; this renders them through the shared <Card>.
// Used both as a home-grid tile (onPress opens the section) and as the folded first tile of its own
// child grid on the detail page — neither shows a chevron/back arrow (nav is via the menu bar).
import React from 'react';
import { View, Text, Pressable } from 'react-native';
import { Card, Flag, VolSlider, HdrBtn, KbdBtn } from '../primitives';
import { s } from '../styles';

export type SynthCardProps = {
  accent?: string; tint?: string;
  letter: string;                // "A" — shown in the media box + kicker
  preset: string;                // hero title: the loaded voice (e.g. "RitChie 1:FM-Rhodes") or "No preset"
  media: string;                 // second hero line: the loaded/playing song (e.g. "♪ 02 Midi Test Chord")
  engine: string;                // Engine flag value (Dexed / OPLL / Plaits …)
  arp: string;                   // Arp flag value ("Simple Up · 1/16" / "Off")
  fx?: string;                   // FX flag value ("Spring 0%"); omit → no FX chip (build without reverb)
  progress?: number;             // playback position (0..1) of whatever's playing (song or loop) — bar + 0-100% above the volume
  connected: boolean;            // disables the controls when not connected
  // USB-keyboard claim glyph (top-right): white = this synth hears the keyboard, grey = it doesn't.
  kbdOwned: boolean; onClaimKbd: () => void;
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
  // Each control is capped at 50px (s.hdrBtnCap) so the row stays compact instead of stretching.
  const actions = (
    <>
      {/* leading spacer pushes the (50px-capped) controls to the right of the card */}
      <View style={{ flex: 1 }} />
      <HdrBtn label="‹ Ins" stop onPress={p.onInsPrev} style={s.hdrBtnCap} />
      <HdrBtn label="Ins ›" stop onPress={p.onInsNext} style={s.hdrBtnCap} />
      <HdrBtn label="▶" onPress={p.onPlay} active={p.playing} style={s.hdrBtnCap} />
      <HdrBtn label="■" stop onPress={p.onStop} style={s.hdrBtnCap} />
      {p.onToggleArp && <HdrBtn label="Arp" onPress={p.onToggleArp} active={!!p.arpOn} style={s.hdrBtnCap} />}
      <HdrBtn label="↻" active={p.repeatOn} onPress={p.onToggleRepeat} style={s.hdrBtnCap} />
    </>
  );
  // Album-cover box: an instrument glyph over the synth's letter, tinted to the track accent.
  const leading = (
    <View style={s.cardMedia}>
      <Text style={s.cardMediaEmoji}>🎹</Text>
      <Text style={[s.cardMediaLetter, p.accent && { color: p.accent }]}>{p.letter}</Text>
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
  const subtitle = (
    <>
      <Text style={s.heroSub} numberOfLines={1}>{p.media}</Text>
      <View style={s.flagRow}>
        <Flag label="Engine" value={p.engine} />
        <Flag label="Arp" value={p.arp} />
        {p.fx && <Flag label="FX" value={p.fx} />}
      </View>
    </>
  );
  return (
    <Card accent={p.accent} tint={p.tint}
      leading={leading} kicker={'Synthesizer ' + p.letter} title={p.preset || 'No preset'} subtitle={subtitle}
      actions={actions} foot={foot}
      topRight={<KbdBtn owned={p.kbdOwned} onPress={p.onClaimKbd} />}
      onPress={p.onPress}
      style={p.onPress || p.gridLead ? s.cardGridAuto : undefined} />
  );
}
