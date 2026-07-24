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

type BeatFeed = { i: number; n: number } | null;

export function Header({
  connected, connecting, onConnectToggle,
  sig, bpm, beatActive, beatFeed,
  vol, onVolChange, onVolCommit,
  status, brandInSidebar,
  metroOn, metroMuted, metroLocked,
  onPlay, onStop, onToggleMute, onStepBpm, onToggleLock,
}: {
  connected: boolean; connecting: boolean; onConnectToggle: () => void;
  sig: number; bpm: number; beatActive: boolean; beatFeed: BeatFeed;
  vol: number; onVolChange: (v: number) => void; onVolCommit: (v: number) => void;
  status: string; brandInSidebar?: boolean;   // desktop w/ sidebar shows the T-DSP logo there → hide it here
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
  // NB: the master transport (▶ ■ 🔊 −/＋BPM 🔒) moved OUT of the header into the nav/menu bar
  // (App.tsx, s.menuTransport). The metro* / onPlay / onStop / onStepBpm / onToggle* props are still
  // accepted for compatibility but no longer rendered here.
  const volBar = (
    <View style={s.volRowHdr}>
      <Text style={s.volLbl}>VOL</Text>
      <ThrottledSlider max={100} value={vol} onChange={onVolChange} onCommit={onVolCommit} disabled={!connected} />
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
        {/* On desktop-with-sidebar the T-DSP logo lives in the sidebar's top-left; here brandSide is
            just a left spacer so beat/VOL stay centered. Elsewhere (disconnected/loading) show the logo. */}
        <View style={s.brandSide}>
          {!brandInSidebar && logo}
        </View>
        {/* Center column: tempo dots on top, master VOL directly beneath them. */}
        <View style={s.brandCenter}>
          {beat}
          {volBar}
        </View>
        <View style={s.brandSideRight}>
          {connectCtrls}
        </View>
      </View>
      <Text style={s.statline}>{status}</Text>
    </View>
  );
}
