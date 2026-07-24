// SideNav.tsx — the section navigation list (Home + each root section with its accent dot and, for
// synth/drum tracks, a USB-keyboard claim glyph). Shared between the desktop left rail and the
// mobile expanded header menu, so both offer the same one-tap navigation. Presentational: the caller
// supplies the sections + handlers. onNavigate fires with the tapped section id (the mobile menu
// wraps it to also close the menu).
import React from 'react';
import { View, Text, Pressable } from 'react-native';
import { C } from './theme';
import { s } from './styles';
import { KbdGlyph } from './primitives';

export type NavSection = { id: string; title: string; accent?: string; disabledReason?: string; kbd?: number; play?: { playing: boolean; onToggle: () => void }; loop?: { on: boolean; onToggle: () => void }; arp?: { on: boolean; onToggle: () => void } };

export function SideNav({ sections, route, activeRootId, onNavigate, usbOwner, onClaimKbd }: {
  sections: NavSection[]; route: string; activeRootId?: string;
  onNavigate: (id: string) => void; usbOwner: (i: number) => boolean; onClaimKbd: (i: number) => void;
}) {
  return (
    <>
      <Pressable style={[s.sideItem, route === 'home' && s.sideItemOn]} onPress={() => onNavigate('home')}>
        <View style={[s.sideDot, { backgroundColor: C.muted }]} />
        <Text style={[s.sideLabel, route === 'home' && s.sideLabelOn]} numberOfLines={1}>Home</Text>
      </Pressable>
      {sections.map(sec => {
        const on = activeRootId === sec.id;
        const ac = sec.accent || C.accent;
        const off = !!sec.disabledReason;
        const kbd = sec.kbd;   // track index for USB-keyboard claim (synths + drum), else undefined
        return (
          <View key={sec.id} style={[s.sideItem, on && s.sideItemOn, on && { borderLeftColor: ac }]}>
            {/* label area navigates; the keyboard glyph is a separate tap target so it never
                triggers a stray navigation */}
            <Pressable style={s.sideItemMain} onPress={() => onNavigate(sec.id)} disabled={off}>
              <View style={[s.sideDot, { backgroundColor: ac }, off && { opacity: 0.4 }]} />
              <Text style={[s.sideLabel, on && s.sideLabelOn, off && s.sideLabelOff]} numberOfLines={1}>{sec.title}</Text>
            </Pressable>
            {kbd != null && !off && (
              <Pressable onPress={() => onClaimKbd(kbd)} hitSlop={6} style={s.sideKbd}
                accessibilityLabel={usbOwner(kbd) ? 'USB keyboard plays this track' : 'Route the USB keyboard to this track'}>
                <KbdGlyph color={usbOwner(kbd) ? C.text : C.muted} />
              </Pressable>
            )}
            {/* per-track Play → Stop toggle: plays this track's loaded song/groove, turns green + ■ while playing */}
            {sec.play && !off && (
              <Pressable onPress={sec.play.onToggle} hitSlop={4} style={[s.navPlayBtn, sec.play.playing && s.navPlayBtnOn]}
                accessibilityLabel={sec.play.playing ? 'Stop' : 'Play'}>
                <Text style={s.navPlayTxt}>{sec.play.playing ? '■' : '▶'}</Text>
              </Pressable>
            )}
            {/* Loop (↻) — toggles this track's song loop (green while looping) */}
            {sec.loop && !off && (
              <Pressable onPress={sec.loop.onToggle} hitSlop={4} style={[s.navPlayBtn, sec.loop.on && s.navPlayBtnOn]}
                accessibilityLabel={sec.loop.on ? 'Looping — tap to stop looping' : 'Loop this song'}>
                <Text style={s.navPlayTxt}>↻</Text>
              </Pressable>
            )}
            {/* Arp — toggles this synth's arpeggiator (green while on) */}
            {sec.arp && !off && (
              <Pressable onPress={sec.arp.onToggle} hitSlop={4} style={[s.navPlayBtn, sec.arp.on && s.navPlayBtnOn]}
                accessibilityLabel={sec.arp.on ? 'Arpeggiator on — tap to turn off' : 'Turn arpeggiator on'}>
                <Text style={s.navArpTxt}>Arp</Text>
              </Pressable>
            )}
          </View>
        );
      })}
    </>
  );
}
