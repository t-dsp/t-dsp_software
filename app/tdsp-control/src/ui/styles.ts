// styles.ts — the shared StyleSheet (`s`) for the T-DSP control surface. Extracted verbatim from
// App.tsx (Phase 1 split); imported by both the leaf primitives and App itself. Depends only on
// the theme palette/constants, so there's no cycle back into the component tree.
import { StyleSheet, Platform } from 'react-native';
import { C, HDR_H, DOWNBEAT, THEME } from './theme';

export const s = StyleSheet.create({
  app: { flex: 1, backgroundColor: C.bg, paddingTop: Platform.OS === 'web' ? 12 : 52 },
  header: { paddingHorizontal: 14, paddingVertical: 8, borderBottomWidth: 1, borderBottomColor: C.border, backgroundColor: C.bg },
  brandRow: { flexDirection: 'row', alignItems: 'flex-start', gap: 8 },
  brandSide: { flex: 1, flexDirection: 'column', alignItems: 'flex-start', gap: 8 },                    // left column: logo on top, transport beneath
  brandLogoRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },                                 // connection dot + brand
  brandCenter: { flex: 1.3, flexDirection: 'column', alignItems: 'center', gap: 6 },                   // center column: tempo dots + VOL beneath
  brandSideRight: { flex: 1, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: 8 },  // right column: reload + connect
  dot: { width: 11, height: 11, borderRadius: 6, backgroundColor: '#da3633' },
  dotOn: { backgroundColor: C.accent },
  brand: { color: C.text, fontWeight: '800', fontSize: 18, letterSpacing: 0.5 },
  // Reload button in the header (web only) — touch-sized, sits just left of Connect.
  refreshBtn: { width: 40, height: 36, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center', marginRight: 8 },
  refreshTxt: { color: C.text, fontSize: 19, fontWeight: '700', lineHeight: 22 },
  statline: { color: C.muted, fontSize: 12, marginTop: 3 },
  // Mobile header (narrow): logo left + beat grid right on the top bar; everything else folds into a menu.
  brandBarMobile: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  menuToggle: { width: 44, height: 40, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center', marginLeft: 4 },
  menuToggleTxt: { color: C.text, fontSize: 20, fontWeight: '700', lineHeight: 22 },
  mobileMenu: { gap: 10, marginTop: 10, paddingTop: 10, borderTopWidth: 1, borderTopColor: C.border },
  mobileConnectRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  // header beat lights (BeatStrip) — one dot per beat of the bar; centered in the header, 2X size
  beatStrip: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 14, height: 28 },
  beatDot: { width: 24, height: 24, borderRadius: 12, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border },
  beatDotDown: { borderColor: 'rgba(227,179,65,0.55)' },   // downbeat marked even when unlit
  beatDotOn: { backgroundColor: C.accent, borderColor: C.accent, shadowColor: C.accent, shadowOpacity: 0.9, shadowRadius: 6, elevation: 4, transform: [{ scale: 1.18 }] },
  beatDotDownOn: { backgroundColor: DOWNBEAT, borderColor: DOWNBEAT, shadowColor: DOWNBEAT, shadowOpacity: 0.9, shadowRadius: 6, elevation: 4, transform: [{ scale: 1.18 }] },
  volRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 2 },
  // Synth-card foot: a mute button to the left of the volume slider (toggles the track to 0%).
  volMuteRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  muteBtn: { width: 40, height: 34, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  muteBtnOn: { backgroundColor: '#da3633', borderColor: '#da3633' },   // muted (track at 0%) — red
  muteTxt: { fontSize: 16, lineHeight: 20 },
  volRowHdr: { flexDirection: 'row', alignItems: 'center', gap: 8, width: '100%' },   // VOL under the beat dots — fills the center column so the slider has room
  volLbl: { color: C.muted, fontSize: 11, width: 26 },
  volVal: { color: C.text, fontSize: 13, width: 28, textAlign: 'right' },
  // Navigation menu bar (below the header): browser-style back / forward / home + current location.
  menuBar: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 8, paddingHorizontal: 14, paddingVertical: 7, borderBottomWidth: 1, borderBottomColor: C.border, backgroundColor: C.card },
  menuTransport: { flexDirection: 'row', alignItems: 'center', gap: 6, marginLeft: 'auto' },   // master transport, pushed to the right of the nav bar (wraps below on narrow)
  menuBtn: { minWidth: 40, height: 32, paddingHorizontal: 12, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  menuBtnText: { color: C.text, fontSize: 16, fontWeight: '700', lineHeight: 18 },
  menuBtnOff: { opacity: 0.35 },   // disabled (no history in that direction)
  menuHere: { color: C.muted, fontSize: 13, fontWeight: '600', marginLeft: 4, flexShrink: 1 },
  // Master transport bar (metronome = the clock): Play / Stop / Mute on the left, BPM on the right.
  transportRow: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6, width: '100%' },   // under the logo; wraps to a 2nd line in a narrow column
  tBtn: { minWidth: 40, height: 34, paddingHorizontal: 10, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  tBtnGhost: { backgroundColor: 'transparent' },
  tBtnOn: { backgroundColor: '#238636', borderColor: '#238636' },   // transport running = lit green
  tBtnText: { color: C.text, fontSize: 16, fontWeight: '700' },
  tBtnOnText: { color: '#fff' },
  tBpm: { color: C.text, fontSize: 18, fontWeight: '800', minWidth: 74, textAlign: 'center' },
  tBpmUnit: { color: C.muted, fontSize: 11, fontWeight: '600' },
  // desktop-only left nav rail: fixed-width column of direct links into each root section
  sideBar: { width: 208, flexGrow: 0, flexShrink: 0, borderRightWidth: 1, borderRightColor: C.border, backgroundColor: C.card2 },
  // T-DSP brand block filling the sidebar's top-left, sized (height 48 + 1px border) to sit level with
  // the menu bar to its right; C.card bg matches the menu bar so the two read as one top strip.
  sideBrand: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 14, height: 48, backgroundColor: C.card, borderBottomWidth: 1, borderBottomColor: C.border },
  content: { flex: 1 },   // main content column beside the rail
  sideItem: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 13, paddingVertical: 11, borderLeftWidth: 3, borderLeftColor: 'transparent' },
  sideItemMain: { flex: 1, flexDirection: 'row', alignItems: 'center', gap: 10 },   // the navigating part of a rail row (dot + label)
  sideKbd: { paddingHorizontal: 4, paddingVertical: 2 },   // compact tap target for the USB-keyboard glyph on track rows
  sideItemOn: { backgroundColor: C.sel },
  sideDot: { width: 9, height: 9, borderRadius: 5 },
  sideLabel: { color: C.muted, fontSize: 14, fontWeight: '600', flexShrink: 1 },
  sideLabelOn: { color: C.text },
  sideLabelOff: { opacity: 0.5 },
  // homepage grid of cards; capped width so it reads well on a wide desktop window too
  home: { flexDirection: 'row', flexWrap: 'wrap', padding: 5, width: '100%' },   // full-width grid (no centered cap)
  cell: { padding: 5 },                                  // grid gutter (width % set inline per column count)
  cardGrid: { marginHorizontal: 0, marginTop: 0, minHeight: 188, flex: 1 },      // flex:1 → stretch to the tallest card in the same grid row (equal-height rows)
  cardGridAuto: { marginHorizontal: 0, marginTop: 0, minHeight: 188, flex: 1 },   // grows to content AND stretches to match the tallest tile in its row
  // Synthesizer-card state summary: one "Label: value" line each (Engine / Preset / MIDI Player / Arp / FX).
  synthSummary: { gap: 1, marginTop: 2 },
  synthSumRow: { color: C.text, fontSize: 12.5 },
  synthSumLabel: { color: C.muted },
  submenu: { flexDirection: 'row', flexWrap: 'wrap' },   // submenu pages tile their cards into a responsive grid
  submenuCell: { padding: 5 },                           // grid gutter for submenu tiles (width % set inline per column count)
  card: { backgroundColor: C.card, borderWidth: 1, borderColor: C.border, borderRadius: 10, marginHorizontal: 10, marginTop: 8, overflow: 'hidden' },
  cardOff: { opacity: 0.45 },                                   // built-but-unavailable feature (see @STATE unavail)
  cardReason: { color: '#f7b955', fontSize: 13, marginTop: 2 }, // amber "⚠ PSRAM required" line on a greyed card
  cardHead: { flexDirection: 'row', alignItems: 'flex-start', gap: 8, paddingHorizontal: 14, paddingTop: 12 },
  // Square media/icon box at the left of a card head (e.g. the Drums card's drum thumbnail).
  cardMedia: { width: 64, height: 64, borderRadius: 8, backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center', overflow: 'hidden' },
  cardMediaIcon: { fontSize: 34, lineHeight: 40, textAlign: 'center' },   // emoji/glyph drum icon inside cardMedia (swap for an <Image> later)
  cardMediaEmoji: { fontSize: 20, lineHeight: 22, textAlign: 'center' },   // small instrument glyph above the synth letter (album-cover style)
  cardMediaLetter: { color: C.text, fontSize: 24, fontWeight: '800', lineHeight: 26, textAlign: 'center' },   // the synth letter (A-D) — tinted to the track accent inline
  cardActions: { flexDirection: 'row', alignItems: 'center', gap: 6, paddingHorizontal: 14, paddingBottom: 12, marginTop: 'auto' },   // pinned to the card bottom
  cardFoot: { paddingHorizontal: 14, paddingTop: 6, paddingBottom: 8, gap: 6 },   // inline controls on the tile (volume slider / reverb on+mix)
  hidden: { display: 'none' },
  drawerLeft: { flex: 1, gap: 2 },
  drawerTitle: { color: C.text, fontWeight: '600', fontSize: 15 },
  drawerTitleHero: { fontSize: 18, marginTop: 1 },   // song-title style hero (track cards with a kicker) — the loaded preset/loop
  cardKicker: { color: C.muted, fontSize: 11, fontWeight: '700', letterSpacing: 0.6, textTransform: 'uppercase' },   // small eyebrow above the hero title (the track's identity: SYNTH A / DRUMS)
  drawerValue: { color: C.accent, fontSize: 14, fontWeight: '600' },
  heroSub: { color: C.accent, fontSize: 13.5, fontWeight: '600', marginTop: 1 },   // the second hero line (drum Kit/Voice under the loop title)
  // Flag chips: a small label+value badge row under the hero (Engine / FX / Arp / Player state at a glance).
  flagRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 6 },
  flag: { flexDirection: 'row', alignItems: 'baseline', gap: 5, backgroundColor: C.chip, borderRadius: 6, borderWidth: 1, borderColor: C.border, paddingHorizontal: 8, paddingVertical: 3, maxWidth: '100%' },
  flagLabel: { color: C.muted, fontSize: 10.5, fontWeight: '700', letterSpacing: 0.4, textTransform: 'uppercase' },
  flagVal: { color: C.text, fontSize: 12.5, fontWeight: '600' },
  pathLine: { color: C.muted, fontSize: 12, marginTop: 1 },
  tag: { color: C.muted, fontSize: 12, backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 2, borderRadius: 10, overflow: 'hidden', alignSelf: 'flex-start' },
  progTrack: { height: 4, borderRadius: 2, backgroundColor: C.chip, marginTop: 6, overflow: 'hidden' },
  progFill: { height: '100%', borderRadius: 2, backgroundColor: C.accent },
  progTrackFoot: { flex: 1, height: 6, borderRadius: 3, backgroundColor: C.chip, overflow: 'hidden' },   // Synth-card foot: playback bar, sized to align with the volume slider's track
  // Looper step grid (bars × beats). Bars are boxed groups; cells are one beat each.
  stepGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 8 },
  stepBar: { flexDirection: 'row', gap: 3, padding: 3, borderRadius: 6, backgroundColor: C.card2, borderWidth: 1, borderColor: C.border },
  stepCell: { width: 15, height: 20, borderRadius: 3, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border },
  stepCellDown: { borderColor: 'rgba(227,179,65,0.5)' },                    // downbeat (beat 1 of the bar)
  stepCellPast: { backgroundColor: 'rgba(63,185,80,0.32)', borderColor: 'rgba(63,185,80,0.4)' },     // played (dim green)
  stepCellOn: { backgroundColor: C.accent, borderColor: C.accent, shadowColor: C.accent, shadowOpacity: 0.9, shadowRadius: 6, elevation: 4 },
  stepCellRecPast: { backgroundColor: 'rgba(248,81,73,0.32)', borderColor: 'rgba(248,81,73,0.4)' },  // captured (dim red)
  stepCellRecOn: { backgroundColor: '#f85149', borderColor: '#f85149', shadowColor: '#f85149', shadowOpacity: 0.9, shadowRadius: 6, elevation: 4 },
  // A tappable "open" button on each card: bordered chip with generous L/R padding so it's an
  // easy target. The ❯ glyph reads as a modern chevron.
  chevBtn: { marginLeft: 'auto', paddingHorizontal: 16, paddingVertical: 8, borderRadius: 9, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  chev: { color: C.text, fontSize: 16, lineHeight: 18, fontWeight: '700' },
  // Keyboard-ownership toggle in a bordered chip button (matches the ❯/❮ nav buttons).
  kbdBtn: { height: HDR_H, paddingHorizontal: 12, borderRadius: 8, backgroundColor: C.chip, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  // section page
  page: { maxWidth: 720, width: '100%', alignSelf: 'center' },
  pageWide: { maxWidth: '100%' },   // submenu-parent pages span the full window so their card grid can use every column
  pageHead: { paddingHorizontal: 14, paddingTop: 12, paddingBottom: 10, borderBottomWidth: 1, borderBottomColor: C.border },
  pageHeadRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  backBtn: { height: HDR_H, paddingHorizontal: 18, borderRadius: 8, borderWidth: 1, borderColor: C.border, backgroundColor: C.chip, alignItems: 'center', justifyContent: 'center' },
  backTxt: { color: C.text, fontSize: 18, fontWeight: '700', lineHeight: 20 },
  pageTitle: { color: C.text, fontWeight: '800', fontSize: 20 },
  pageBody: { paddingHorizontal: 14, paddingTop: 14, gap: 8 },
  // Synth/Voices: fill-height picker with a breadcrumb nav bar above it
  synthWrap: { flex: 1, gap: 10 },
  navBar: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  upBtn: { width: 38, height: 38, borderRadius: 8, borderWidth: 1, borderColor: C.border, alignItems: 'center', justifyContent: 'center' },
  upBtnOff: { opacity: 0.35 },
  upTxt: { color: C.text, fontSize: 22, fontWeight: '700', lineHeight: 24 },
  crumbs: { flex: 1 },
  crumbsInner: { alignItems: 'center', paddingRight: 8 },
  crumbItem: { flexDirection: 'row', alignItems: 'center' },
  crumbSep: { color: C.muted, fontSize: 15, marginHorizontal: 4 },
  crumbTxt: { color: C.accent, fontSize: 14, fontWeight: '600' },
  crumbLast: { color: C.text, fontSize: 14, fontWeight: '700' },
  picker: { flex: 1, borderWidth: 1, borderColor: C.border, borderRadius: 7 },
  row: { flexDirection: 'row', alignItems: 'center', gap: 6, flexWrap: 'wrap' },
  muted: { color: C.muted, fontSize: 13 },
  sectionLbl: { color: C.text, fontSize: 13, fontWeight: '700', marginTop: 10, marginBottom: 2 },
  text: { color: C.text, fontSize: 14 },
  btn: { backgroundColor: '#238636', paddingHorizontal: 12, paddingVertical: 8, borderRadius: 7, alignItems: 'center' },
  btnWide: { marginTop: 4 },
  // prominent "Connect App" call-to-action shown on the home screen when disconnected
  connectHome: { marginTop: 56, paddingHorizontal: 24, alignItems: 'center' },
  connectBig: { paddingVertical: 16, paddingHorizontal: 44, minWidth: 240 },
  connectBigText: { color: C.text, fontSize: 17, fontWeight: '700' },
  // Transport picker (connect screen): segmented USB/Bluetooth | Wi-Fi + optional host box.
  segRow: { flexDirection: 'row', borderWidth: 1, borderColor: C.border, borderRadius: 8, overflow: 'hidden', marginBottom: 14 },
  seg: { paddingVertical: 9, paddingHorizontal: 22, backgroundColor: C.card2, minWidth: 104, alignItems: 'center' },
  segOn: { backgroundColor: C.sel },
  segText: { color: C.muted, fontSize: 13, fontWeight: '600' },
  segTextOn: { color: C.text },
  hostInput: { width: 260, marginBottom: 6, textAlign: 'center' },
  hostHint: { color: C.muted, fontSize: 11, textAlign: 'center', maxWidth: 300, marginBottom: 16 },
  // Discovered-device list (mDNS)
  devWrap: { width: 280, marginBottom: 12 },
  devHead: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 8, marginBottom: 8 },
  devRow: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 7, paddingVertical: 8, paddingHorizontal: 12, marginBottom: 6 },
  devRowOn: { borderColor: C.accent, backgroundColor: C.sel },
  devName: { color: C.text, fontSize: 14, fontWeight: '600' },
  devAddr: { color: C.muted, fontSize: 11, marginTop: 2 },
  // catalog loading screen (connected, not yet loaded)
  loadWrap: { flex: 1, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 32, gap: 14 },
  loadTitle: { color: C.text, fontSize: 17, fontWeight: '700' },
  loadTrack: { width: '100%', maxWidth: 360, height: 8, borderRadius: 4, backgroundColor: C.chip, overflow: 'hidden' },
  loadFill: { height: '100%', borderRadius: 4, backgroundColor: C.accent },
  loadSub: { color: C.muted, fontSize: 13, textAlign: 'center' },
  loadHint: { color: C.muted, fontSize: 11, textAlign: 'center', opacity: 0.8 },
  grow1: { flex: 1 },
  headActions: { flexDirection: 'row', alignItems: 'center', gap: 6, flexShrink: 0 },   // content-sized → buttons keep natural width on the page header
  hdrActionsRow: { flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', gap: 8, paddingHorizontal: 14, paddingBottom: 12, marginTop: -2 },
  hdrBtn: { backgroundColor: '#238636', height: HDR_H, paddingHorizontal: 7, borderRadius: 8, flexGrow: 1, flexShrink: 1, flexBasis: 0, minWidth: 34, alignItems: 'center', justifyContent: 'center' },
  // Player-control buttons (the ‹ › ▶ ■ transport rows on the synth cards + MIDI Player / Arp / FX):
  // capped width so the row stays compact, and a 50px-tall touch target. hdrBtnTall = the same 50px
  // height without the width cap, for wider text buttons (Synth/Voices' ‹ Prev / Next ›).
  hdrBtnCap: { maxWidth: 50, height: 50 },
  hdrBtnTall: { height: 50 },
  hdrBtnRec: { backgroundColor: '#da3633', borderWidth: 0 },   // loop-record transport button while armed/capturing (red)

  hdrBtnStop: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  hdrBtnIdle: { backgroundColor: C.chip },   // play ▶ when NOT playing: dark (green only while playing)
  hdrBtnText: { color: C.text, fontSize: 13, fontWeight: '700' },
  btnGhost: { backgroundColor: 'transparent', borderWidth: 1, borderColor: C.border },
  btnOn: { backgroundColor: '#238636', borderWidth: 2, borderColor: C.accent },   // PLAYING: green fill + bright border
  btnIdle: { backgroundColor: C.chip },               // NOT playing: dark fill (overrides s.btn green) so the state reads at a glance
  btnRecOn: { borderWidth: 2, borderColor: THEME.recorder.accent },   // armed / capturing (red)
  btnText: { color: C.text, fontSize: 13, fontWeight: '600' },
  input: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 7, color: C.text, paddingHorizontal: 10, paddingVertical: 8, fontSize: 14 },
  list: { maxHeight: 300, borderWidth: 1, borderColor: C.border, borderRadius: 7 },
  browseBox: { height: 340 },   // fixed height so <FolderBrowser>'s flex picker lays out inside a card body
  listBtn: { paddingHorizontal: 12, paddingVertical: 10, borderBottomWidth: 1, borderBottomColor: C.border },
  listBtnSel: { backgroundColor: C.sel },
  // Pattern picker: a wrapping GRID so every one of the 26 patterns is reachable at once
  // (a horizontal strip hid the ones past the first row). Cells stretch to fill each row.
  // Preset/Manual segmented tabs — one arp editor active at a time.
  arpTabs: { flexDirection: 'row', backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 9, padding: 3, gap: 3, marginTop: 2 },
  arpTab: { flex: 1, alignItems: 'center', paddingVertical: 9, borderRadius: 7 },
  arpTabOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  arpTabTxt: { color: C.muted, fontSize: 14, fontWeight: '700' },
  arpTabTxtOn: { color: C.text },
  patGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 2 },
  patCell: { backgroundColor: C.chip, paddingHorizontal: 8, paddingVertical: 9, borderRadius: 8, minWidth: 74, flexGrow: 1, flexBasis: 74, alignItems: 'center' },
  patCellOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  patCellTxt: { color: C.text, fontSize: 13, fontWeight: '600' },
  pill: { backgroundColor: C.chip, paddingHorizontal: 12, paddingVertical: 6, borderRadius: 14 },
  pillOn: { backgroundColor: C.sel, borderWidth: 1, borderColor: C.accent },
  statGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  stat: { backgroundColor: C.card2, borderWidth: 1, borderColor: C.border, borderRadius: 8, paddingVertical: 10, paddingHorizontal: 12, minWidth: 96, flexGrow: 1, alignItems: 'center' },
  statN: { color: C.text, fontSize: 22, fontWeight: '800' },
  statL: { color: C.muted, fontSize: 12, marginTop: 2 },
  statSub: { color: C.accent, fontSize: 11, marginTop: 1 },
});
