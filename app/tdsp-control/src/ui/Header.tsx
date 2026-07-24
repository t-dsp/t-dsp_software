// Header.tsx — the global app header shown on every page.
//
// Desktop (wide): a three-column grid — brand + transport on the left, the centered tempo dots
// with the master VOL directly beneath (center), reload/connect on the right, status line below.
//
// Mobile (narrow, width < 700): the logo stays top-left and the tempo/beat grid sits top-right;
// everything else (transport, VOL, reload/connect, status) collapses into a menu opened by the ☰
// toggle. Keeps the cramped little controls off the phone header until you ask for them.
//
// Purely presentational: App owns all the state and passes the live values + handlers down; the
// header just composes the leaf components (BeatStrip, ThrottledSlider). The only local state is
// whether the mobile menu is open.
import React, { useState } from 'react';
import { View, Text, Pressable, Platform, useWindowDimensions } from 'react-native';
import { s } from './styles';
import { BeatStrip } from './elements/BeatStrip';
import { ThrottledSlider } from './primitives';
import { SideNav, NavSection } from './SideNav';

type BeatFeed = { i: number; n: number } | null;

export function Header({
  connected, connecting, onConnectToggle,
  sig, bpm, beatActive, beatFeed,
  vol, onVolChange, onVolCommit,
  status, brandInSidebar, nav,
  metroOn, metroMuted, metroLocked,
  onPlay, onStop, onToggleMute, onStepBpm, onToggleLock,
}: {
  connected: boolean; connecting: boolean; onConnectToggle: () => void;
  sig: number; bpm: number; beatActive: boolean; beatFeed: BeatFeed;
  vol: number; onVolChange: (v: number) => void; onVolCommit: (v: number) => void;
  status: string; brandInSidebar?: boolean;   // desktop w/ sidebar shows the T-DSP logo there → hide it here
  // Section navigation data (same list the desktop rail shows) — rendered inside the mobile ☰ menu.
  nav?: { sections: NavSection[]; route: string; activeRootId?: string; navigate: (id: string) => void; usbOwner: (i: number) => boolean; claimUsb: (i: number) => void };
  metroOn: boolean; metroMuted: boolean; metroLocked: boolean;
  onPlay: () => void; onStop: () => void; onToggleMute: () => void; onStepBpm: (d: number) => void; onToggleLock: () => void;
}) {
  const { width } = useWindowDimensions();
  const narrow = width < 700;
  const [open, setOpen] = useState(false);

  // ---- shared pieces (composed differently on mobile vs desktop) ---------------------------------
  const logo = (
    <View style={s.brandLogoRow}>
      <View style={[s.dot, connected && s.dotOn]} />
      <Text style={s.brand}>T-DSP</Text>
    </View>
  );
  const beat = <BeatStrip sig={sig} bpm={bpm} active={beatActive} live={beatFeed} />;
  // Master transport, split into two rows: play/stop/mute on top, the BPM stepper + tempo lock beneath.
  // Rendered in the header's top-left on desktop, and stacked in the ☰ menu on mobile.
  const transportPlay = (
    <View style={s.hdrTransportRow}>
      <Pressable style={[s.tBtn, metroOn && s.tBtnOn]} onPress={onPlay} disabled={!connected}
        accessibilityLabel={metroOn ? 'Transport running — restart the downbeat' : 'Start the transport'}>
        <Text style={[s.tBtnText, metroOn && s.tBtnOnText]}>▶</Text></Pressable>
      <Pressable style={[s.tBtn, s.tBtnGhost]} onPress={onStop} disabled={!connected} accessibilityLabel="Stop everything">
        <Text style={s.tBtnText}>■</Text></Pressable>
      <Pressable style={[s.tBtn, s.tBtnGhost, !metroMuted && s.tBtnOn]} disabled={!connected} onPress={onToggleMute}
        accessibilityLabel={metroMuted ? 'Click muted — tap to hear it' : 'Click audible — tap to mute'}>
        <Text style={[s.tBtnText, !metroMuted && s.tBtnOnText]}>{metroMuted ? '🔇' : '🔊'}</Text></Pressable>
    </View>
  );
  const transportBpm = (
    <View style={s.hdrTransportRow}>
      <Pressable style={s.tBtn} onPress={() => onStepBpm(-1)} disabled={!connected}><Text style={s.tBtnText}>−</Text></Pressable>
      <Text style={s.tBpm}>{Math.round(bpm)}<Text style={s.tBpmUnit}> BPM</Text></Text>
      <Pressable style={s.tBtn} onPress={() => onStepBpm(1)} disabled={!connected}><Text style={s.tBtnText}>＋</Text></Pressable>
      <Pressable style={[s.tBtn, s.tBtnGhost, metroLocked && s.tBtnOn]} disabled={!connected} onPress={onToggleLock}
        accessibilityLabel={metroLocked ? 'Tempo locked — tap to let content set the BPM' : 'Tempo follows content — tap to lock'}>
        <Text style={[s.tBtnText, metroLocked && s.tBtnOnText]}>{metroLocked ? '🔒' : '🔓'}</Text></Pressable>
    </View>
  );
  const volBar = (
    <View style={s.volRowHdr}>
      <Text style={s.volLbl}>VOL</Text>
      {/* Taller drag zone (48px) than the default 34 — the master VOL is a touchscreen target. */}
      <ThrottledSlider max={100} value={vol} onChange={onVolChange} onCommit={onVolCommit} disabled={!connected} style={{ flex: 1, height: 48 }} />
      <Text style={s.volVal}>{Math.round(vol)}</Text>
    </View>
  );
  // Reload button (web only): app-window Chrome on the jay-mint touchscreen has no browser toolbar,
  // so this is the tap target to pull the latest Metro bundle after a code change.
  const connectCtrls = (
    <>
      {Platform.OS === 'web' && (
        <Pressable style={s.refreshBtn} onPress={() => (globalThis as any).location?.reload?.()} accessibilityLabel="Reload the app">
          <Text style={s.refreshTxt}>⟳</Text>
        </Pressable>
      )}
      <Pressable style={s.btn} onPress={onConnectToggle}>
        <Text style={s.btnText}>{connected ? 'Disconnect App' : connecting ? 'Cancel' : 'Connect App'}</Text>
      </Pressable>
    </>
  );

  // ---- MOBILE: logo left · beat grid right · everything else behind the ☰ toggle ------------------
  if (narrow) {
    return (
      <View style={s.header}>
        <View style={s.brandBarMobile}>
          {logo}
          <View style={{ flex: 1 }} />
          {beat}
          <Pressable style={[s.menuToggle, open && s.tBtnOn]} onPress={() => setOpen(o => !o)}
            accessibilityLabel={open ? 'Hide controls' : 'Show controls'}>
            <Text style={s.menuToggleTxt}>{open ? '✕' : '☰'}</Text>
          </Pressable>
        </View>
        {open && (
          <View style={s.mobileMenu}>
            {/* master transport (two rows: play/stop/mute, then BPM), under the logo/beat/✕ top bar */}
            {transportPlay}
            {transportBpm}
            {/* the left-hand section navigation (same list as the desktop rail); tapping also closes the menu */}
            {nav && (
              <View style={s.mobileNav}>
                <SideNav sections={nav.sections} route={nav.route} activeRootId={nav.activeRootId}
                  onNavigate={id => { nav.navigate(id); setOpen(false); }} usbOwner={nav.usbOwner} onClaimKbd={nav.claimUsb} />
              </View>
            )}
            {volBar}
            <View style={s.mobileConnectRow}>{connectCtrls}</View>
            {!!status && <Text style={s.statline}>{status}</Text>}
          </View>
        )}
      </View>
    );
  }

  // ---- DESKTOP: the three-column layout ----------------------------------------------------------
  return (
    <View style={s.header}>
      <View style={s.brandRow}>
        {/* Top-left: the master transport — play/stop/mute on top, BPM stepper + lock beneath. (The
            T-DSP logo lives in the sidebar's top-left on desktop; shown here only when there's no sidebar.) */}
        <View style={s.brandSide}>
          {!brandInSidebar && logo}
          {transportPlay}
          {transportBpm}
        </View>
        {/* Center column: tempo dots on top, master VOL directly beneath them. */}
        <View style={s.brandCenter}>
          {beat}
          {volBar}
        </View>
        <View style={s.brandSideRight}>
          <View style={s.brandConnectRow}>{connectCtrls}</View>
          {/* status line sits directly under the reload/connect buttons (was a full-width line below). */}
          {!!status && <Text style={s.statlineRight} numberOfLines={2}>{status}</Text>}
        </View>
      </View>
    </View>
  );
}
