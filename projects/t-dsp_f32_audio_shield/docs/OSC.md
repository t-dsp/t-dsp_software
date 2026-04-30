# T-DSP OSC specification

Source-of-truth OSC protocol for the T-DSP F32 audio-shield mixer firmware
(Teensy 4.1 + TAC5212 codec). This document is the contract between the firmware,
the dev-surface UI, and any third-party client (Mixing Station, TouchOSC, custom).

This spec is opinionated: where a concept maps cleanly onto the Behringer X32 OSC
dialect, we copy X32 verbatim. Anyone with X32 muscle memory should be able to
read this spec and immediately understand the address tree, the type tags, and
the value ranges.

The reference for X32 conventions is Patrick-Gilles Maillot, "Unofficial X32/M32
OSC Remote Protocol", v4.06-09, March 17 2022 (henceforth "X32 OSC PDF" — page
numbers cite that document).

---

## 1. Overview

### 1.1 What this protocol is

The mixer is a small-format Teensy 4.1 + TAC5212 board that stands in for a
single channel-strip slice of an X32: USB host playback, on-board PDM mics, line
inputs, optional XLR (TLV320ADC6140), an FM synth (Dexed) bus, and a stereo
master with high-shelf processing and host-volume tracking.

The dev surface (and any third-party client) talks to the mixer over a single
**OSC 1.0** message stream. The protocol is **stateless at the wire level** —
every leaf is independently readable, writable, and subscribable. There are no
sessions, no logins, no version handshake beyond the `/info` reply.

### 1.2 Transport

- **Wire**: OSC 1.0 messages and bundles, framed with **SLIP** (RFC 1055), over
  the Teensy's **USB CDC Serial** endpoint at 115200 baud (rate is decorative;
  CDC ignores it). One SLIP frame contains exactly one OSC packet (a `Message`
  or a `Bundle`).
- **Why SLIP-OSC over CDC**: USB CDC is reliable, byte-stream, and exposed to
  the host as a normal serial port. SLIP gives us atomic packet boundaries
  without TCP overhead. The Teensy 4.1 has neither Ethernet nor Wi-Fi
  out of the box, so UDP/OSC isn't an option without extra hardware.
- **No multicast, no broadcast, no auth.** The firmware accepts every well-
  formed packet that arrives on the CDC stream.

### 1.3 Why X32-modeled

X32 has the most widely deployed OSC dialect in pro audio. Mixing Station, X32
Edit, M32 Edit, TouchOSC's X32 layouts, every X32 utility on GitHub: they all
speak the same address tree. By copying X32 wherever a concept maps directly,
we get free interop with that ecosystem and we don't have to write our own UI
muscle memory.

The deviations (no DCAs initially, fewer channels, `/synth/...` and `/codec/...`
extensions, single-fader main bus, no Ethernet) are documented inline and
collected in section 17.

### 1.4 Document conventions

- A **leaf** is a single OSC address with a fixed type-tag list.
- A **branch** is an address prefix that contains leaves and/or further branches.
- A "tag" in this doc means an OSC type-tag character (`i`, `f`, `s`, `b`).
- Address segments are ASCII, lowercase, no separators. See section 2.
- When a leaf is described as `,if` or `,si`, the comma-prefix follows the OSC 1.0
  type-tag-string convention; in normal prose we drop the comma.

---

## 2. Conventions

### 2.1 Type tags

The mixer supports the OSC 1.0 core types only.

| Tag | C type            | Range / encoding                          | Used for                                |
|-----|-------------------|-------------------------------------------|-----------------------------------------|
| `i` | int32 (BE)        | -2^31..+2^31-1                            | booleans (0/1), enum codes, MIDI bytes  |
| `f` | float32 (IEEE BE) | -inf..+inf, NaN treated as 0              | faders, gains, frequencies, dB          |
| `s` | OSC-string        | NUL-terminated ASCII, padded to 4 bytes   | names, voice/bank labels, "ON"/"OFF"    |
| `b` | OSC-blob          | int32 length + raw bytes, 4-byte aligned  | reserved for batched meter packets      |

**No timetags.** Bundle timetag is always `1` (immediate). Receivers ignore the
timetag byte order. Senders that care about ordering should rely on the byte-
stream property of CDC, not the bundle timetag.

**No int64, no double, no `T`/`F`/`N`/`I` shorthand types.** A dev-surface
client that emits these gets a silent drop on the firmware side.

### 2.2 Boolean leaves

X32 represents booleans as enum-32 with the `OffOn` enum. We follow that, with
one tweak: the wire type is `,i` (int32), values 0 or 1.

- **Read**: server replies with `,i 0` or `,i 1`.
- **Write**: server accepts:
  - `,i 0` or `,i 1` (canonical),
  - `,i n` where n != 0 → coerced to 1,
  - `,s "OFF"` / `,s "ON"` (case-insensitive) for X32 Edit interop,
  - any other type → silently dropped.

### 2.3 Address style

- All segments lowercase ASCII. No hyphens, no underscores, no camelCase.
- Numeric instance indices are **zero-padded to the family's max width**.
  Today: channels are `01..10` (width 2). Mixer-grade gear traditionally uses
  width 2; we keep that even though we have only six channels wired. If a
  family ever needs 100+ instances, we'd widen that family alone (not retro-
  pad existing ones).
- Singular branch for instances: `/ch/01`, `/fxrtn/01`, `/synth/dexed`.
- Plural branch for **lists** (X32 convention): `/meters/0`, `/-libs`, `/dca`.
  Today the mixer has no DCAs and no meter family — the convention is reserved.
- **Verbs sit at the top of the tree**: `/info`, `/status`, `/save`, `/load`,
  `/snapshot`, `/node`, `/subscribe`, `/renew`, `/unsubscribe`, `/xremote`.
- **`/-` prefix marks system / housekeeping branches** (X32 convention:
  `/-stat`, `/-prefs`, `/-libs`). Parameters under a `/-` branch are not part
  of the user's mix state — they're console-internal status, capture-side
  hardware, FW version, etc. A scene save (`/save`, future) does not include
  `/-` paths.

### 2.4 Fader law

For all leaves named `mix/fader`, all leaves named `mix/NN/level` (channel-to-
bus send levels), and the mirrored `/-cap/hostvol/value`, the wire format is
a normalized float `[0.0, 1.0]` mapping piecewise-linearly to `[-inf, +10] dB`.

The four-segment mapping is the X32 fader law (X32 OSC PDF, p. 132):

```c
// float-to-dB
// f: [0.0, 1.0]   d: [-inf, +10] dB   (f == 0.0 yields -inf, conventionally -90)

if      (f >= 0.5)    d = f * 40.0  - 30.0;   // -10 .. +10  dB
else if (f >= 0.25)   d = f * 80.0  - 50.0;   // -30 .. -10  dB
else if (f >= 0.0625) d = f * 160.0 - 70.0;   // -60 .. -30  dB
else                  d = f * 480.0 - 90.0;   // -inf .. -60 dB

// dB-to-float
// d: [-90, +10] dB   f: [0.0, 1.0]

if      (d < -60.0)   f = (d + 90.0) / 480.0;
else if (d < -30.0)   f = (d + 70.0) / 160.0;
else if (d < -10.0)   f = (d + 50.0) /  80.0;
else /* d <= 10 */    f = (d + 30.0) /  40.0;
```

**Quantization snap** (X32 OSC PDF, p. 132). After computing `f` from `dB`, the
firmware optionally rounds to one of the X32-known fader steps:

```c
// 1024 steps for fader leaves (mix/fader, mix/01/level, etc.)
f = (int)(f * 1023.5) / 1023.0;
```

For the channel-to-bus `mix/NN/level` leaves, X32 uses a coarser **161-step**
grid. We mirror that:

```c
// 161 steps for send-level leaves
f = (int)(f * 160.5) / 160.0;
```

Senders may emit any 32-bit float; the firmware snaps on the way in, and the
echo reports the snapped value.

**Cardinal points** for sanity-checking a sender:

| dB    | float    | Note                                         |
|-------|----------|----------------------------------------------|
| -inf  | 0.0000   | minimum                                       |
| -90   | 0.0000   | min reportable (X32 caps -inf at -90)         |
| -60   | 0.0625   | seg-1/seg-2 boundary                          |
| -30   | 0.2500   | seg-2/seg-3 boundary                          |
| -10   | 0.5000   | seg-3/seg-4 boundary                          |
|   0   | 0.7500   | unity                                         |
| +10   | 1.0000   | maximum                                       |

A more granular table appears in section 14.

**dB units that are NOT this fader law.** Preamp trim, EQ band gain, dynamics
threshold, and codec output `dvol` all use plain dB on the wire — `,f` followed
by a number in dB. Only the `mix/fader` and `mix/NN/level` family use the X32
4-segment law.

### 2.5 Pan

Pan is a single float on `[-1.0, +1.0]`, where `-1.0` is full-left, `0.0` is
center, `+1.0` is full-right. X32 reports pan with the same convention. When a
channel is stereo-linked (`/config/chlink/N-M ,i 1`), writes to `pan` on the
left member mirror to the right member with sign **flipped** (so a "spread"
gesture on a coupled pair widens both sides simultaneously). See section 12 for
the full link-mirror table.

### 2.6 Echo behavior

Every successful write emits an immediate **echo bundle** to the writing client:
the leaf address with its post-write value. The echo is sent regardless of
whether the client has `/xremote` registered — it's the way the dev surface
confirms a write took, and it lets a "fire and forget" client validate without
holding a subscription open.

Failures are silent on the wire. Bad type tags, out-of-range floats, unknown
addresses, and malformed messages all drop with a `Serial.println()` log line
on the firmware side. There is **no** error-reply OSC address.

### 2.7 Ranges and clamping

Every numeric leaf has a documented min/max in section 18 (Address index). The
firmware **clamps** out-of-range writes to the nearest endpoint and echoes the
clamped value. Clients should rely on the echo, not on the wire round-trip
matching their original value.

### 2.8 Naming — addresses vs UI labels

OSC addresses are mechanical: `/ch/05/mix/fader`. The human label for that
channel ("Mic L" or whatever the user types in) lives at `/ch/05/config/name`
and is independent from the address. Renaming a channel does NOT change its
OSC address.

### 2.9 Scene state vs runtime state

Following X32, "scene state" means everything that gets saved/restored by
`/save` and `/load` (future hooks, see section 3). Today that's the entire
`/ch`, `/auxin`, `/fxrtn`, `/main`, `/synth`, `/codec`, `/arp`, and
`/config` tree. **Not** scene state: anything under `/-` (housekeeping),
`/meters` (live data), or `/snapshot` (on-demand reply).

---

## 3. Discovery and the control plane

### 3.1 `/info`

```
->  /info
<-  /info ,ssss "t-dsp_f32_audio_shield" "phase2" "TAC5212-Teensy41" "fw 0.2.0"
```

**Reply**: four `,s` fields:
1. product name
2. firmware phase / channel string (e.g. "phase2", "phase3a", "production")
3. hardware model token
4. firmware semver `fw X.Y.Z`

The reply matches X32's `/info` shape (X32 OSC PDF, p. 9): version, server name,
console model, console version. Clients that already parse X32 `/info` keep
working.

### 3.2 `/status`

```
->  /status
<-  /status ,sss "active" "USB:CDC" "1.0.0"
```

`,sss`: state token, transport descriptor, protocol version. State is one of:
`active`, `booting`, `flashing`. Transport is the human-readable wire ("USB:CDC"
on this hardware; future Wi-Fi build would emit `"UDP:192.168.x.y"`).

### 3.3 `/snapshot`

The legacy bulk-state read. Returns one big `OSCBundle` containing every
user-visible leaf value at the moment of the call. Useful on dev-surface
connect — issue `/snapshot` once, populate the UI, then `/xremote` for live
updates.

`/snapshot` is **not** the same as `/node` (section 3.8). `/node` is X32's
group-read primitive (one packed text line per subtree); `/snapshot` is a
flat dump of every leaf. Long-term, the dev surface should migrate to
`/node`-based partial loads. `/snapshot` is kept for backwards compatibility.

### 3.4 `/xremote`

```
->  /xremote
<-  (no immediate reply; server registers caller for 10-second push window)
```

The X32 push-on-change subscription. Once registered, every leaf write emits
to up to **four active clients** for a window of **10 seconds**. The client
must re-issue `/xremote` before the window expires to remain registered.
Following X32 OSC PDF, p. 9.

A few notes on what `/xremote` does and doesn't push:

- **Pushes**: every leaf write made by anyone (including this client itself,
  the dev surface, or a third client). Echoes for your own writes still come
  back through `/xremote`.
- **Pushes**: synthetic state changes (codec register read-back changes from
  the panel poll loop, hostvol changes from Windows, MIDI viz events).
- **Does not push**: meter updates. Meters are subscription-only via
  `/subscribe ,si /meters/0 [tf]` (X32 OSC PDF, pp. 79-80, paragraph "Meter
  requests"). Today there's no `/meters/...` family; reserved for later.
- **Does not push**: pure read replies to `/snapshot`, `/node`, `/info`,
  `/status`. Those go to the requester regardless of `/xremote`.

If five clients try to register simultaneously, the **fifth** client is
rejected silently. There is no `/xremote_full` reply.

### 3.5 `/subscribe`

The per-leaf polled subscription. Format (X32 OSC PDF, pp. 7-8):

```
/subscribe ,s  "<address>"             # default rate: every 50ms (rate-code 1)
/subscribe ,si "<address>" <rate>      # explicit rate (see below)
```

`<address>` is the full leaf path (with leading `/`). `<rate>` is one of the
**four X32 rate codes**, an integer modulo factor of the 5ms server tick:

| Code | Modulo | Period | Approx Hz | Use case                       |
|------|--------|--------|-----------|---------------------------------|
| 1    | every  | 50 ms  | 20 Hz     | meter strip, fader follow       |
| 10   | /10    | 500 ms | 2 Hz      | "slow" parameters               |
| 50   | /50    | 2.5 s  | 0.4 Hz   | settings / status panels        |
| 100  | /100   | 5 s    | 0.2 Hz   | once-every-window heartbeat    |

Codes outside `{1, 10, 50, 100}` are clamped to the nearest valid code. The
subscription expires after 10 seconds; renew via `/renew`.

**What's subscribable**: any leaf in the address tree. Subscriptions on
non-existent addresses are accepted silently and produce no reports.

**What's NOT subscribable**: branch addresses (e.g. `/subscribe ,s "/ch/01"`).
Use `/node` for grouped reads instead.

### 3.6 `/renew`

```
->  /renew ,s "<previous-subscribe-address>"
```

Resets the 10-second TTL on a previously issued `/subscribe`. The subscription
must still exist (i.e. you're inside the 10 s window). If it has already
lapsed, you must re-issue the original `/subscribe` — `/renew` does not
recreate from scratch.

`/xremote` has its own renewal: just re-issue `/xremote`.

### 3.7 `/unsubscribe`

```
->  /unsubscribe ,s "<address>"
```

Releases a `/subscribe`. There is no `/xremote` counterpart — `/xremote`
expires by TTL only. If a client wants to drop its push registration before
the TTL, it can: just stop renewing.

### 3.8 `/node`

X32's group-read primitive. Returns one **packed text line per subtree leaf**,
NUL-padded and bundled.

```
->  /node ,s "/ch/01/mix"
<-  /node ,s "fader 0.7500"
<-  /node ,s "on 1"
<-  /node ,s "pan 0.0"
<-  /node ,s "st 1"
```

Each reply line is `"<leaf-name> <value>"`, with the leaf-name relative to
the requested branch. Multiple values on one leaf are space-separated. The
reply is one OSC bundle containing `N` `/node` messages, where `N` is the
number of leaves under the requested branch.

`/node` is most useful for "give me the EQ block in one round-trip" or "give
me everything in `/synth/dexed`".

### 3.9 `/save` and `/load` (future)

```
/save ,si "<scene-name>" <slot>      # save current state to slot
/load ,i  <slot>                     # restore from slot
```

Reserved for the future SD-card scene system. Not implemented today; firmware
will reply with `/save ,s "unsupported"` etc. Documented here so the namespace
is locked.

---

## 4. Channel strip — `/ch/[01..10]/...`

The mixer has **six channels wired today** with **slots reserved for ten total**.
Per X32 convention, the entire channel-strip leaf surface exists for every
channel slot; for channels with no audio path wired, writes are accepted and
echoed (state mutates) but have no audio effect. This means a dev surface can
populate UI from `/snapshot` without conditional code per channel.

### 4.1 Channel map

| ch     | Wire today          | Audio path                                              | Notes                       |
|--------|---------------------|---------------------------------------------------------|-----------------------------|
| `01`   | yes                 | USB host playback L → mixL slot 0                      | unmuted at boot             |
| `02`   | yes                 | USB host playback R → mixR slot 0                      | unmuted at boot             |
| `03`   | reserved            | TAC5212 ADC CH1 (Line L); slot allocated, no F32 wire   | leaf accepts/echoes writes  |
| `04`   | reserved            | TAC5212 ADC CH2 (Line R); slot allocated, no F32 wire   | leaf accepts/echoes writes  |
| `05`   | yes                 | TAC5212 PDM mic L → mixL slot 2                        | **muted at boot**           |
| `06`   | yes                 | TAC5212 PDM mic R → mixR slot 2                        | **muted at boot**           |
| `07`   | future              | TLV320ADC6140 XLR 1                                    | leaf accepts writes only    |
| `08`   | future              | TLV320ADC6140 XLR 2                                    | leaf accepts writes only    |
| `09`   | future              | TLV320ADC6140 XLR 3                                    | leaf accepts writes only    |
| `10`   | future              | TLV320ADC6140 XLR 4                                    | leaf accepts writes only    |

PDM mics default-muted because the on-board condensers are LIVE the moment
their channels are powered. A user has to actively toggle `/ch/05/mix/on 1`
to start monitoring.

### 4.2 Address tree

```
/ch/NN/
    config/
        name        ,s   1..12 chars         user label (free text)
        icon        ,i   0..74               X32 icon enum (display hint, not enforced)
        color       ,i   0..14               X32 color enum (see 4.4)
        source      ,i   0..255              hardware source code (see 4.5)
    preamp/
        trim        ,f   -18.0..+60.0 dB    digital input trim
        invert      ,i   0/1                phase invert (post-trim)
        hpf/
            on      ,i   0/1                high-pass filter engage
            f       ,f   20.0..400.0 Hz     HPF corner (X32 standard)
            slope   ,i   0..3               12/18/24/36 dB/oct (X32 enum)
    mix/
        on          ,i   0/1                channel mute (1 = unmuted)
        fader       ,f   0.0..1.0           X32 fader law -> -inf..+10 dB
        pan         ,f   -1.0..+1.0         L..C..R (mirrors with sign-flip on link)
        st          ,i   0/1                send to /main/st bus
        mono        ,i   0/1                send to /main/m bus (no-op on this hw)
        mlevel      ,f   0.0..1.0           mono-bus send level (no-op on this hw)
        NN/                                 (channel-to-bus sends, NN=01..16)
            on      ,i   0/1                send-on
            level   ,f   0.0..1.0           161-step send law (see 2.4)
            pan     ,f   -1.0..+1.0
            type    ,i   0..3               pre/post/sub-group (X32 Xmtype enum)
    grp/
        dca         ,i   0..255 (bitmask)   DCA group membership (no-op until DCAs)
        mute        ,i   0..63  (bitmask)   mute-group membership
    eq/
        on          ,i   0/1                EQ section engage
        N/                                  (N = 1..4)
            type    ,i   0..5               LCut/LShlv/PEQ/HCut/HShlv/VEQ (X32 Xeqty1)
            f       ,f   20.0..20000.0 Hz   center / corner
            g       ,f   -15.0..+15.0 dB   gain (PEQ + shelves)
            q       ,f   0.3..10.0          Q (PEQ) / slope (shelves)
    dyn/                                    (placeholder for future on-DSP comp/gate)
        on          ,i   0/1
        thr         ,f   -60.0..0.0 dB
        ratio       ,i   0..11              X32 Xdyrat enum
        attack      ,f   0.0..120.0 ms
        release     ,f   5.0..4000.0 ms
        mgain       ,f   0.0..+24.0 dB
```

### 4.3 What's wired vs reserved (today)

The firmware code path that actually mutates audio for each leaf, by channel:

| Leaf                          | ch01,02 | ch03,04 | ch05,06 | ch07-10 |
|-------------------------------|---------|---------|---------|---------|
| `config/name,icon,color,src`  | state   | state   | state   | state   |
| `preamp/trim`                 | --      | --      | --      | --      |
| `preamp/hpf/{on,f,slope}`     | --      | --      | --      | --      |
| `mix/on`                      | wired   | --      | wired   | --      |
| `mix/fader`                   | wired   | --      | wired   | --      |
| `mix/pan`                     | --      | --      | --      | --      |
| `mix/st`                      | --      | --      | --      | --      |
| `eq/N/...`                    | --      | --      | --      | --      |
| `dyn/...`                     | --      | --      | --      | --      |

`--` = leaf accepted, value echoed, no audio side effect today. State is
preserved across `/snapshot` so a dev surface UI doesn't lose the user's
intent when the firmware later wires that path.

`state` = mutates a per-channel struct kept in memory. Not yet persisted
across reboot — when `/save` lands, the entire `/ch` tree gets serialized.

### 4.4 Color enum

X32 `Xcolors` (X32 OSC PDF, channel config table):

| Code | Name      | RGB hint         |
|------|-----------|------------------|
| 0    | OFF       | (no color)       |
| 1    | RD        | red              |
| 2    | GN        | green            |
| 3    | YE        | yellow           |
| 4    | BL        | blue             |
| 5    | MG        | magenta          |
| 6    | CY        | cyan             |
| 7    | WH        | white            |
| 8    | OFFi      | off + inverted   |
| 9    | RDi       | red inverted     |
| 10   | GNi       | green inverted   |
| 11   | YEi       | yellow inverted  |
| 12   | BLi       | blue inverted    |
| 13   | MGi       | magenta inverted |
| 14   | CYi       | cyan inverted    |
| 15   | WHi       | white inverted   |

The dev surface picks display RGB from the code; the firmware just stores it.

### 4.5 Source enum

Following X32, `config/source` codes the routed hardware source. The mixer
maps source codes to a **subset** of X32's table — only the codes that
correspond to real ports on this board. Writes of unknown codes are clamped
to 0 (UNUSED) and logged.

| Code  | Name       | Hardware                                   |
|-------|------------|--------------------------------------------|
| 0     | OFF        | unused / cleared                           |
| 1     | USB-L      | USB host playback L                        |
| 2     | USB-R      | USB host playback R                        |
| 3     | LINE-L     | TAC5212 ADC CH1                            |
| 4     | LINE-R     | TAC5212 ADC CH2                            |
| 5     | MIC-L      | TAC5212 PDM mic L                          |
| 6     | MIC-R      | TAC5212 PDM mic R                          |
| 7-10  | XLR1..XLR4 | TLV320ADC6140 inputs (when wired)          |
| 11    | SYNTH-L    | internal synth bus L (rare; bypass routing)|
| 12    | SYNTH-R    | internal synth bus R                       |

`source` is a free **routing** parameter — by default, channel `NN` is mapped
to source code `NN` (so `/ch/01` reads USB-L, etc.), but the user can repatch.
Repatching is **future** today; currently only the default mapping works.

### 4.6 Stereo link interaction

When `/config/chlink/1-2 ,i 1` is set, `/ch/01` and `/ch/02` are coupled —
writes to `mix/fader`, `mix/on`, `preamp/*`, `eq/*` mirror across, and
`mix/pan` mirrors with sign-flip. See section 12 for the full table.

---

## 5. Aux ins and FX returns — `/auxin`, `/fxrtn`

X32 has 8 aux-ins and 8 FX returns. The mixer has neither aux ins (no
extra physical inputs beyond the channel set) nor most FX returns. We
keep the namespace for forward compatibility.

### 5.1 `/auxin/[01..08]`

**Today: not wired.** Leaf surface mirrors `/ch/01` (config/preamp/mix/eq).
Writes are accepted and echoed; no audio effect. Reserved for a future build
that adds line-level aux returns.

### 5.2 `/fxrtn/[01..08]`

**Today: only `/fxrtn/01` is wired**, and it's **the synth bus return**.

The synth bus group fader (`g_synthBusL`, `g_synthBusR` in main.cpp) sits
between every per-synth gain and the mix bus. We expose it under
`/fxrtn/01` so the dev surface — and any X32-ish client — can drive it
through the standard FX-return strip.

Address tree for `/fxrtn/01`:

```
/fxrtn/01/
    config/
        name        ,s        default "Synth"
        color       ,i        default 5 (MG)
    mix/
        on          ,i        synth bus on (mirrors /synth/bus/on)
        fader       ,f        synth bus level (X32 fader law, mirrors /synth/bus/fader)
```

The two paths (`/fxrtn/01/mix/...` and `/synth/bus/...`) read and write the
same underlying state; whichever the client uses, the firmware echoes back on
the path the client wrote to.

`/fxrtn/02..08` exist as leaves but are reserved.

### 5.3 FX-return link

`/config/fxlink/1-2 ,i 1` couples `/fxrtn/01` and `/fxrtn/02` as a stereo
pair. With only `/fxrtn/01` wired, this is purely cosmetic today.

---

## 6. Main bus — `/main/st/...`

This hardware has a **single stereo main**, no separate mono bus. We expose
`/main/st` (the stereo bus). `/main/m` (mono) returns "unsupported" on every
write — the leaf is documented but not implemented.

### 6.1 Address tree

```
/main/st/
    config/
        name        ,s        default "Main"
        color       ,i        default 4 (BL)
    mix/
        on          ,i        master mute (1 = unmuted)
        fader       ,f        master fader (X32 law -> -inf..+10 dB)
        link        ,i        L/R link (default 1; see 6.3)
        faderL      ,f        L-channel fader (when link=0)
        faderR      ,f        R-channel fader (when link=0)
        pan         ,f        -1..+1 (no-op on this hardware)
    hostvol/                  Windows USB Audio Class FU 0x31 (playback slider)
        on          ,i        engage hostvol stage in graph
        value       ,f        0.0..1.0 (read-back from USB FU; writable for testing)
    eq/
        on          ,i        master EQ engage (currently mapped to /proc/shelf/enable)
        N/                    (N = 1..4; only band 1 mapped today, to high-shelf)
            type    ,i        0..5 (LCut/LShlv/PEQ/HCut/HShlv/VEQ)
            f       ,f        20..20000 Hz
            g       ,f        -15..+15 dB
            q       ,f        0.3..10.0
    dyn/                      reserved for codec on-chip limiter
        on          ,i
        thr         ,f
```

### 6.2 Today's wiring (proc/shelf compatibility shim)

The firmware currently exposes the high-shelf at three legacy addresses for
compatibility with the existing dev surface UI:

```
/proc/shelf/enable   ,i      same state as /main/st/eq/on
/proc/shelf/freq     ,f      same state as /main/st/eq/1/f
/proc/shelf/gain     ,f      same state as /main/st/eq/1/g
```

These are **deprecated** in favor of `/main/st/eq/...` and will be removed
once the dev surface migrates. Both addresses read and write the same state;
both echo back to whichever path the client wrote.

The default values match the production "Dull" preset:
- `/main/st/eq/on = 1`
- `/main/st/eq/1/type = 4` (HShlv)
- `/main/st/eq/1/f = 3000.0`
- `/main/st/eq/1/g = -12.0`
- `/main/st/eq/1/q = 0.7` (mapped to slope=1.0 in the F32 biquad)

### 6.3 Main-fader link

`mix/link` controls whether `mix/fader` is the canonical master (link=1) or
whether L and R are independent (`mix/faderL` / `mix/faderR`, link=0).

- `link=1` (default): `mix/fader` writes set both internal `g_mainFaderL`
  and `g_mainFaderR`. Reads of `mix/faderL` / `mix/faderR` return the
  shared value.
- `link=0`: `mix/faderL` and `mix/faderR` are independently writable.
  `mix/fader` reads return the average; writes update both equally (i.e.
  it's a "trim" gesture in unlinked mode).

Today's hardware has `mix/link` always 1 implicitly (the firmware drives
both `g_mainFaderL` and `g_mainFaderR` together in `/main/st/mix/faderL`
and `/main/st/mix/faderR` handlers, with `mix/fader` not wired). The leaf
surface is documented so a future "trim" feature can land without breaking
clients.

### 6.4 Hostvol behavior

`hostvol/on` controls whether the Windows playback slider attenuates the
master output. When on, the firmware polls
`AudioInputUSB_F32::volume()` every loop tick, applies a square-law taper
(matches "log-pot feel"), and pushes into `hostvolAmpL` / `hostvolAmpR`.

When off, the hostvol stage is held at unity (1.0) — the dev-surface fader
becomes the only volume control and the Windows slider is ignored.

`hostvol/value` is normally **read-only** from the client's perspective — it
echoes whatever the latest USB poll reported. Writes from the client are
accepted (for testing / forced-attenuation) but the next poll tick overwrites
unless `hostvol/on = 0`.

---

## 7. Synth subtree — `/synth/...`

The mixer hosts **one** synth engine today (Dexed) with the architecture for
multiple. The address tree separates per-engine state (`/synth/dexed`) from
the bus (`/synth/bus`).

### 7.1 `/synth/dexed`

```
/synth/dexed/
    config/
        name        ,s            default "Dexed"
        color       ,i            default 5 (MG)
    mix/
        on          ,i            engine mute (1 = unmuted)
        fader       ,f            per-engine level (X32 law)
    midi/
        ch          ,i            0=omni, 1..16=specific MIDI channel
        program     ,i            0..127 last program-change received (read-mostly)
    voice/
        bank        ,i            0..(kNumBanks-1)        (currently 0..9, Ritchie banks)
        program     ,i            0..(kVoicesPerBank-1)   (0..31, per bank)
        name        ,s            10-char DX7 voice name (read-only echo)
    bank/
        names       ,s s s ...    one ,s per bank, queryable list
    voices/
        names       ,is...        ,i bank then ,s per voice; query with ,i bank
```

Notes:

- `/synth/dexed/voice` accepts a **paired write**: `,ii bank voice`. Reply
  is `,iis bank voice name` (the firmware looks up the canonical voice name
  from `dexed_banks_data.h`).
- `/synth/dexed/bank/names` is **read-only**; any write is ignored. The
  reply lists every bank name in order.
- `/synth/dexed/voices/names` takes a single `,i bank` argument and returns
  the 32 voice names for that bank. Sending without an argument returns the
  current bank's names.

**Compatibility aliases** (deprecated, kept for the existing dev surface):

| Legacy address                       | Canonical address                  |
|--------------------------------------|------------------------------------|
| `/synth/dexed/volume`                | `/synth/dexed/mix/fader`           |
| `/synth/dexed/on`                    | `/synth/dexed/mix/on`              |
| `/synth/dexed/midi/ch`               | (kept; canonical)                  |
| `/synth/dexed/voice ,ii`             | (kept; canonical)                  |
| `/synth/dexed/bank/names`            | (kept; canonical)                  |
| `/synth/dexed/voice/names`           | `/synth/dexed/voices/names`        |

The firmware accepts both old and new addresses; echoes go to whichever the
client wrote.

### 7.2 `/synth/bus`

The synth-bus group fader. Sits downstream of every per-engine fader and
upstream of the mix bus.

```
/synth/bus/
    config/
        name        ,s            default "Synth"
        color       ,i            default 5 (MG)
    mix/
        on          ,i            bus mute (1 = unmuted)
        fader       ,f            bus level (X32 law)
```

Same state as `/fxrtn/01/mix/...` — see 5.2.

**Compatibility aliases** (legacy linear-volume API):

| Legacy             | Canonical                |
|--------------------|--------------------------|
| `/synth/bus/volume`| `/synth/bus/mix/fader`   |
| `/synth/bus/on`    | `/synth/bus/mix/on`      |

### 7.3 Future synth engines

When a second engine lands (MPE poly, Neuro, etc.), it gets its own
`/synth/<name>/` subtree with the same shape as `/synth/dexed`. The bus
fader continues to serve the sum of all engines.

The dev surface enumerates engines by `/node ,s "/synth"`, then traverses
each child branch.

---

## 8. MIDI — `/midi/...`

### 8.1 Outbound viz (server -> client broadcast)

```
/midi/note ,iii <note> <velocity> <channel>
```

Broadcast (via `/xremote`) on every note-on / note-off seen at the
`MidiVizSink` (post-router, post-arp). `velocity == 0` is note-off.
`channel` is the MIDI channel (1..16). Sent only when
`/midi/viz/on = 1` to keep CDC quiet when the dev surface isn't watching.

`/midi/viz/on ,i` toggles broadcasting. Default 1 (on).

### 8.2 Inbound — UI keyboard (client -> server)

```
/midi/note/in ,iii <note> <velocity> <channel>
```

Used by the dev surface's on-screen keyboard. Feeds the same router as the
USB-host keyboard, so synth sinks see UI notes and hardware notes
identically. `velocity > 0` is note-on; `velocity == 0` is note-off.

The firmware does **not** echo this leaf — it's a one-way trigger.

### 8.3 Inbound — control change

```
/midi/cc/in ,iii <cc-number> <value> <channel>
```

Future. Today only note-on/off is wired from the dev surface. Reserved.

### 8.4 Outbound — clock pulse (optional)

`/midi/clock` is reserved for a future "tick visualizer" — broadcasts the
24-PPQN tick stream. Not implemented today.

---

## 9. Arpeggiator — `/arp/...`

```
/arp/
    on          ,i        engage arp filter (default 0 = passthrough)
    rate        ,i        16th=4, 8th=8, 4th=16, etc. (X32-style ticks-per-step)
    pattern     ,i        0=up, 1=down, 2=updown, 3=random, 4=as-played
    octaves     ,i        1..4 octave-spread
    gate        ,f        0.05..1.0 gate length (fraction of step)
    swing       ,f        0.0..0.5 (0 = straight, 0.5 = max triplet feel)
    latch       ,i        0/1 hold-mode
    bpm         ,f        20..300 (only used when /clock/source = internal)
```

`/arp/on 1` arms the filter; `/arp/on 0` is pure passthrough (every event
forwarded verbatim — clock, viz, etc. all see raw MIDI regardless).

### 9.1 Clock source

```
/clock/source ,i      0=internal, 1=external-MIDI
/clock/bpm    ,f      mirrors /arp/bpm when source=internal
```

The arp uses the shared `tdsp::Clock`. When source is internal, the firmware
emits 24-PPQN ticks at `bpm`. When external, ticks come from the USB-host
MIDI keyboard's `0xF8` byte stream (when present).

---

## 10. Codec panels — `/codec/...`

These are **hardware-register** panels exposed through the existing
`Tac5212Panel` and (future) `Adc6140Panel` classes. They are NOT user-
facing mix state — they're the codec engineer's view into chip registers,
with all the gain stages, ADC mode bits, PDM clock dividers, limiter
coefs, and so on.

### 10.1 `/codec/tac5212/...`

Routed by `tdsp::OscDispatcher` to `Tac5212Panel`. The full leaf surface is
maintained in `projects/t-dsp_f32_audio_shield/src/Tac5212Panel.cpp` — refer
there for the canonical list.

High-level summary of the namespace:

```
/codec/tac5212/
    info/                    chip-id, rev, status registers
    serial/                  TDM/I2S format, slot offsets, channel-to-slot routing
    adc/N/                   N=1..2; mode (single-ended/diff), gain, mute, slot
    dac/N/                   N=1..2; volume, route, mute, mode
    out/N/                   N=1..2; OUTxP/M mode, hp-driver enable, dvol
    in/N/                    N=1..4; channel-enable, source select
    pdm/                     PDM mic enable, clock divider, slot routing
    dsp/                     limiter / DRC / BOP coefficient blocks (PPC3-only)
    interrupts/              IRQ mask, latched flags
    initialize               ,i  trigger full re-init (matches "Initialize" UI button)
```

Limiter coefficient editing is **PPC3-only** — the encoding format isn't
user-friendly enough to expose as raw OSC. The dev surface greys out
`/codec/tac5212/dsp/limiter/...` editing on this hardware.

See the Tac5212Panel source for register-level documentation.

### 10.2 `/codec/adc6140/...`

Future. When the TLV320ADC6140 wires in, this branch exposes its register
panel. Same shape as `/codec/tac5212`, scoped to that chip's gain/mode/slot
controls.

---

## 11. System and housekeeping — `/-stat`, `/-cap`, `/-prefs`

The `/-` prefix marks branches that are **not** part of user mix state.
Scenes don't include these; clients don't typically edit them.

### 11.1 `/-stat/...`

```
/-stat/
    cpu                ,f       0.0..100.0 % main-loop CPU usage (read-only)
    audio/
        blocks/
            f32        ,ii      <inUse> <max>   F32 audio block pool
            i16        ,ii      <inUse> <max>   int16 audio block pool
    fw/
        version        ,s       "0.2.0"          read-only
        phase          ,s       "phase2"         build label
        gitref         ,s       short SHA        read-only
    boot/
        timeMs         ,i       millis since boot (read-only, monotonic)
        reason         ,s       "cold", "softreset", "watchdog"
    usbhost/
        connected      ,i       0/1 (any USB-host MIDI device present)
        deviceName     ,s       last enumerated USB-MIDI product string
```

`/-stat/...` leaves are subscribable but mostly slow-changing; rate code 50
(2.5 s) is a sensible default.

### 11.2 `/-cap/hostvol/...`

Capture-side host volume — Windows USB Audio Class FU 0x30 (recording slider).

```
/-cap/hostvol/
    value      ,f       0.0..1.0   read-only echo of Windows recording slider
    mute       ,i       0/1        read-only echo of Windows recording mute
```

These are tracked **but not yet consumed** by the audio graph. They feed
listenback monitor amps in a later phase. The dev surface displays a "CAP HOST"
strip in the Mixer view backed by these leaves.

**Compatibility alias** (legacy address): `/usb/cap/hostvol/value` and
`/usb/cap/hostvol/mute`. Both addresses read the same state. The firmware
emits on the legacy path today; the canonical `/-cap/...` form is the
forward-compatible name.

### 11.3 `/-prefs/...`

Future. Persistent device preferences (saved to QSPI flash on shutdown):

```
/-prefs/
    autoLoad        ,i       0/1   load slot N at boot
    bootSlot        ,i       1..N  scene slot to load
    midiChannel/
        thru        ,i       0/1   forward USB-host MIDI to USB-device MIDI
    ledBrightness   ,f       0.0..1.0
```

Not implemented; the namespace is reserved.

---

## 12. Stereo-link semantics

Stereo link is **global config**, not per-channel state. Three link leaves
exist on the mixer today:

```
/config/chlink/1-2     ,i 0/1
/config/chlink/3-4     ,i 0/1
/config/chlink/5-6     ,i 0/1
```

For aux ins / fx returns / future buses:

```
/config/auxlink/1-2   ,i 0/1
/config/fxlink/1-2    ,i 0/1
```

For DCAs / matrices / mute groups (future):

```
/config/buslink/1-2   ,i 0/1
/config/mtxlink/1-2   ,i 0/1
```

### 12.1 What link-on does

When `/config/chlink/1-2 ,i 1` is set:

- The two channels become a **coupled pair**, "ch01" being the L member and
  "ch02" being the R member.
- A write to most leaves on EITHER side **mirrors server-side** to the other
  side, with the firmware emitting **both** echoes (so any subscribed client
  sees both updates).
- A `/snapshot` returns both sides' current values, which will be identical
  for the mirrored leaves and may differ for un-mirrored leaves.

### 12.2 Mirror table

| Leaf                            | Mirrored?  | Sign-flip on R member? |
|---------------------------------|------------|------------------------|
| `mix/fader`                     | yes        | no                     |
| `mix/on`                        | yes        | no                     |
| `mix/st`                        | yes        | no                     |
| `mix/pan`                       | yes        | **yes** (X32 spread)   |
| `mix/NN/level`                  | yes        | no                     |
| `mix/NN/on`                     | yes        | no                     |
| `mix/NN/pan`                    | yes        | **yes**                |
| `preamp/trim`                   | yes        | no                     |
| `preamp/invert`                 | yes        | no                     |
| `preamp/hpf/on`                 | yes        | no                     |
| `preamp/hpf/f`                  | yes        | no                     |
| `preamp/hpf/slope`              | yes        | no                     |
| `eq/on`                         | yes        | no                     |
| `eq/N/type`                     | yes        | no                     |
| `eq/N/f`                        | yes        | no                     |
| `eq/N/g`                        | yes        | no                     |
| `eq/N/q`                        | yes        | no                     |
| `dyn/...`                       | yes        | no                     |
| `grp/dca`                       | yes        | no                     |
| `grp/mute`                      | yes        | no                     |
| `config/icon`                   | yes        | no                     |
| `config/color`                  | yes        | no                     |
| `config/name`                   | **NO**     | --                     |
| `config/source`                 | **NO**     | --                     |

`config/name` and `config/source` are **independent per side** — the user
might label them "Bass DI L" / "Bass DI R" or even completely different names,
and the source routing is a per-channel hardware patch.

### 12.3 Worked example

Initial state:

```
/config/chlink/1-2     ,i 0           # unlinked
/ch/01/mix/fader       ,f 0.7500
/ch/02/mix/fader       ,f 0.7500
/ch/01/mix/pan         ,f -0.5         # 50% left
/ch/02/mix/pan         ,f +0.5         # 50% right
/ch/01/config/name     ,s "Bass DI L"
/ch/02/config/name     ,s "Bass DI R"
```

Client sends:

```
/config/chlink/1-2     ,i 1
```

Server replies (echoes) to the writing client and pushes via `/xremote`:

```
/config/chlink/1-2     ,i 1
```

State after link-on: no values change yet. Link only mirrors **subsequent**
writes, not retroactively. The two channels keep their independent values.

Now client sends:

```
/ch/01/mix/fader       ,f 0.5000
```

Server processes: clamps/snaps to nearest 1024-step value (≈ 0.5005),
applies to internal `g_chFader[1]`, AND because chlink/1-2 is on, also
applies to `g_chFader[2]`. Echoes to writing client and pushes to all
`/xremote` subscribers:

```
/ch/01/mix/fader       ,f 0.5005
/ch/02/mix/fader       ,f 0.5005
```

Now client sends a pan write:

```
/ch/01/mix/pan         ,f -0.7
```

Server applies `-0.7` to ch01 AND mirrors with sign flip to ch02:

```
/ch/01/mix/pan         ,f -0.7
/ch/02/mix/pan         ,f +0.7
```

Now client sends a name change:

```
/ch/01/config/name     ,s "Bass"
```

`config/name` does NOT mirror. Server echoes:

```
/ch/01/config/name     ,s "Bass"
```

`/ch/02/config/name` is still "Bass DI R" — unchanged.

### 12.4 Link-off behavior

`/config/chlink/1-2 ,i 0` un-couples the pair. Subsequent writes do not
mirror. Existing values stay where they were when the link was on (no
reversion). The two channels are now independent.

### 12.5 Edge cases

- **Writing to ch02 directly while linked**: the firmware mirrors back to
  ch01 the same way — link is symmetric. The L member is the "canonical"
  side only by convention; the firmware doesn't enforce which side initiates.
- **Conflicting writes within one frame**: if a single bundle contains
  `/ch/01/mix/fader 0.5` AND `/ch/02/mix/fader 0.6`, the firmware applies
  them in order; the second write (with mirror) wins, leaving both at 0.6.
  The echo bundle reflects that.

---

## 13. Configuration tree — `/config/...`

Mirrors X32's `/config` subtree, scoped to what this hardware actually has.

```
/config/
    chlink/
        1-2         ,i      0/1
        3-4         ,i      0/1
        5-6         ,i      0/1
        7-8         ,i      0/1   (reserved for XLR pair)
        9-10        ,i      0/1   (reserved for XLR pair)
    auxlink/
        1-2         ,i      0/1   (reserved)
    fxlink/
        1-2         ,i      0/1   (synth bus pair, today single-mono)
        3-4         ,i      0/1   (reserved)
    mute/                    mute groups (reserved; no DCA initially)
        1           ,i      0/1   group-1 master mute
        2           ,i      0/1   group-2 master mute
        ...                  (up to 6, X32 standard)
    routing/                 future patchbay matrix (reserved)
        IN          ,is...   input routing block
        OUT         ,is...   output routing block
    osc/                     test oscillator (reserved; X32 has /config/osc)
        on          ,i
        f           ,f
        level       ,f
    solo/                    solo-bus config (reserved)
    talk/                    talkback (no hardware; reserved)
    tape/                    tape-return routing (no hardware; reserved)
```

**Today, only the `chlink/...` and `fxlink/1-2` leaves are functionally
wired.** Other leaves accept and echo but have no audio effect.

Default state at boot:

```
/config/chlink/1-2 ,i 1       # USB L/R linked (typical)
/config/chlink/3-4 ,i 1       # Line L/R linked (typical)
/config/chlink/5-6 ,i 1       # Mic L/R linked (typical)
/config/fxlink/1-2 ,i 1       # synth bus pair linked (cosmetic; only fxrtn/01 wired)
```

A user can break links from the dev surface (Setup tab) for cases where
they want independent control over a "stereo" pair.

---

## 14. Fader law — full appendix

This is the X32 4-segment piecewise-linear fader law lifted directly from
the X32 OSC PDF, p. 132. We use it for **every** leaf named `mix/fader`
and every leaf named `mix/NN/level`.

### 14.1 Float-to-dB

```c
// Input  f: [0.0, 1.0]  (OSC float on the wire)
// Output d: [-90, +10]  (dB; f == 0 maps to -inf conventionally, -90 numerically)

double x32_float_to_db(double f) {
    if      (f >= 0.5)    return f * 40.0  - 30.0;   // -10 .. +10  dB
    else if (f >= 0.25)   return f * 80.0  - 50.0;   // -30 .. -10  dB
    else if (f >= 0.0625) return f * 160.0 - 70.0;   // -60 .. -30  dB
    else                  return f * 480.0 - 90.0;   // -inf .. -60 dB
}
```

### 14.2 dB-to-float

```c
// Input  d: [-90, +10]  (dB)
// Output f: [0.0, 1.0]

double x32_db_to_float(double d) {
    if      (d < -60.0)   return (d + 90.0) / 480.0;
    else if (d < -30.0)   return (d + 70.0) / 160.0;
    else if (d < -10.0)   return (d + 50.0) /  80.0;
    else /* d <= 10 */    return (d + 30.0) /  40.0;
}
```

### 14.3 Quantization

X32 fader leaves snap to a **1024-step** grid (10-bit):

```c
double x32_fader_snap(double f) {
    return ((int)(f * 1023.5)) / 1023.0;
}
```

Channel-to-bus send `level` leaves snap to a **161-step** grid:

```c
double x32_send_snap(double f) {
    return ((int)(f * 160.5)) / 160.0;
}
```

The firmware applies the snap on writes; clients should use the echo as the
authoritative post-snap value.

### 14.4 Cardinal points

Already shown briefly in 2.4; full reference table:

| dB     | float    | dB     | float    | dB     | float    |
|--------|----------|--------|----------|--------|----------|
| -inf   | 0.0000   | -29.0  | 0.2625   | -1.0   | 0.7250   |
| -90.0  | 0.0000   | -28.0  | 0.2750   | 0.0    | 0.7500   |
| -84.0  | 0.0125   | -27.0  | 0.2875   | +1.0   | 0.7750   |
| -78.0  | 0.0250   | -26.0  | 0.3000   | +2.0   | 0.8000   |
| -72.0  | 0.0375   | -25.0  | 0.3125   | +3.0   | 0.8250   |
| -66.0  | 0.0500   | -24.0  | 0.3250   | +4.0   | 0.8500   |
| -60.0  | 0.0625   | -22.5  | 0.3438   | +5.0   | 0.8750   |
| -57.0  | 0.0813   | -20.0  | 0.3750   | +6.0   | 0.9000   |
| -54.0  | 0.1000   | -18.0  | 0.4000   | +7.0   | 0.9250   |
| -51.0  | 0.1187   | -16.0  | 0.4250   | +8.0   | 0.9500   |
| -48.0  | 0.1375   | -14.0  | 0.4500   | +9.0   | 0.9750   |
| -45.0  | 0.1563   | -12.0  | 0.4750   | +10.0  | 1.0000   |
| -42.0  | 0.1750   | -10.0  | 0.5000   |        |          |
| -39.0  | 0.1937   | -8.0   | 0.5500   |        |          |
| -36.0  | 0.2125   | -6.0   | 0.6000   |        |          |
| -33.0  | 0.2313   | -4.0   | 0.6500   |        |          |
| -30.0  | 0.2500   | -2.0   | 0.7000   |        |          |

For granular checks, consult the X32 OSC PDF, pp. 133-135 — full 0.0625-step
table.

### 14.5 Notes

- **`-inf` representation**: the wire never carries `-inf`. A fader at the
  bottom emits `,f 0.0`. Clients that want to render "−∞" should display
  `-∞` whenever the dB value computed from `f` is less than or equal to
  `-90.0`.
- **+10 dB clamp**: writes of `,f` greater than 1.0 clamp to 1.0 (= +10 dB).
  Writes less than 0.0 clamp to 0.0.
- **Per-stage internal scale**: the firmware translates the snapped float
  to a linear gain `g = pow(10, dB/20)` before pushing into the F32
  `AudioEffectGain_F32` stage. The dB is intermediate; the linear gain is
  what the audio graph consumes.

---

## 15. Subscription deep-dive

This section describes the subscription primitives at the precision level a
firmware author needs to reproduce the behavior. The reference is X32 OSC
PDF, pp. 79-80 (paragraph "Subscribing to X32/M32 Updates") and pp. 7-9.

### 15.1 The four primitives

| OSC address       | Purpose                              | Per-leaf granularity? |
|-------------------|--------------------------------------|-----------------------|
| `/xremote`        | "send me everything that changes"    | NO (all-or-nothing)   |
| `/subscribe`      | "send me this one leaf at rate R"    | YES                   |
| `/renew`          | refresh `/subscribe` TTL             | YES                   |
| `/unsubscribe`    | cancel a `/subscribe`                | YES                   |

Plus two batch primitives that the mixer **does not implement** (yet):
`/formatsubscribe` and `/batchsubscribe`. They're documented in X32 OSC PDF
pp. 7-8 for clients that care; the mixer rejects them silently. Reserved.

### 15.2 `/xremote` lifecycle

```
client -> server: /xremote
server: notes the client's CDC stream as a registered consumer; starts a
        10-second TTL timer.
server -> client: (no reply)
... time passes, leaves change ...
server -> client: every leaf write echoes here (in a bundle, if multiple
                 changes happened in the same audio-update tick).
... after 10s if not renewed ...
server: drops the client from the registered list.

To stay registered: re-issue /xremote every < 10 seconds.
Recommended cadence: every 8 seconds.
```

Maximum **four** simultaneous `/xremote` consumers. CDC is single-client in
practice (only one host can open the COM port at a time), but the four-
client cap is preserved for forward-compat with a future Wi-Fi build.

### 15.3 `/subscribe` rate codes

From X32 OSC PDF, p. 8. The server tick is **5 ms**. The rate code is a
modulo factor:

| Code  | 5 ms ticks per update | Effective period | Approx Hz |
|-------|----------------------|------------------|-----------|
| 1     | 1                    | 5 ms             | 200 Hz    |
| 10    | 10                   | 50 ms            | 20 Hz     |
| 50    | 50                   | 250 ms           | 4 Hz      |
| 100   | 100                  | 500 ms           | 2 Hz      |

(X32 PDF examples use 200/4/4/2 updates per 10 s, which is what the math
yields.)

For values outside `{1, 10, 50, 100}`, the firmware clamps to the nearest
listed code. A code of 0 is treated as 1 (smallest rate).

**Mixer simplification**: today the firmware does not actually implement a
5 ms server-tick subscription engine — `/subscribe` only echoes every
**leaf change** (effectively rate-code 1, change-driven). The dev surface
treats this as "good enough" because the meters family isn't wired yet.
The full rate-code engine lands when meters do.

### 15.4 What's subscribable

Every leaf in the address tree is subscribable. **Branches are not** —
`/subscribe ,s "/ch/01/mix"` is rejected silently.

For a "give me the whole channel" workflow, use `/node`:

```
/node ,s "/ch/01"          # one-shot read of every leaf under /ch/01
```

Combined with `/xremote`, this is the canonical pattern for "populate UI
once, then receive live updates":

```
client -> /node ,s "/ch/01"
client <- /node ,s "config/name 'USB-L'"
client <- /node ,s "mix/fader 0.7500"
... (full subtree) ...
client -> /xremote
... live updates ...
```

### 15.5 Push semantics (echoes)

When the mixer applies a write, it composes an echo bundle containing
**every** leaf that changed. For most writes that's a single leaf. For
linked writes (section 12), it's two. For codec-panel writes that touch
multiple registers atomically, it can be more.

The echo bundle is sent to:
- The writing client (always).
- All `/xremote`-registered clients (if any).
- All `/subscribe`-registered clients whose subscribed address matches one
  of the changed leaves.

A single state change does NOT generate three separate sends to the same
client; the firmware deduplicates.

---

## 16. Worked examples

### 16.1 Reading a single fader

```
client -> /ch/01/mix/fader
server -> /ch/01/mix/fader ,f 0.7500
```

A read is just a write with no payload. The server replies with the current
value.

### 16.2 Writing a fader

```
client -> /ch/01/mix/fader ,f 0.5
server -> /ch/01/mix/fader ,f 0.5005   # snapped to nearest 1024-step value
```

### 16.3 Group read via `/node`

```
client -> /node ,s "/ch/01/mix"
server -> (bundle: 5 messages)
            /node ,s "on 1"
            /node ,s "fader 0.5005"
            /node ,s "pan 0.0000"
            /node ,s "st 1"
            /node ,s "mono 0"
```

### 16.4 Subscribing to a level meter

(meters are reserved; here for the future shape)

```
client -> /subscribe ,si "/meters/0/ch/01" 1
server -> /meters/0/ch/01 ,f -23.4    # ~ every 50 ms for 10 s
        /meters/0/ch/01 ,f -22.8
        ...
client -> /renew ,s "/meters/0/ch/01"
        ... another 10 s ...
client -> /unsubscribe ,s "/meters/0/ch/01"
```

### 16.5 Registering for all updates (`/xremote`)

```
client -> /xremote
        (no reply; client is now in the push list for 10 s)

(somewhere else, the user moves a fader on the codec panel)
server -> /codec/tac5212/out/1/dvol ,f -3.0       # pushed to client

(another client connects and changes a channel name)
server -> /ch/02/config/name ,s "Vox"             # also pushed to first client

(client renews before TTL expires)
client -> /xremote
```

### 16.6 Linked-pair fader move

Initial: `/config/chlink/1-2 = 1`.

```
client -> /ch/01/mix/fader ,f 0.5
server -> (bundle to writing client + all /xremote clients):
            /ch/01/mix/fader ,f 0.5005
            /ch/02/mix/fader ,f 0.5005
```

### 16.7 Linked-pair pan with sign-flip

```
client -> /ch/01/mix/pan ,f -0.7
server -> (bundle):
            /ch/01/mix/pan ,f -0.7
            /ch/02/mix/pan ,f +0.7
```

### 16.8 Synth voice change

```
client -> /synth/dexed/voice ,ii 3 12
server -> /synth/dexed/voice ,iis 3 12 "BRASS    1"
```

The firmware loads bank 3 voice 12, panics any held notes on the engine,
echoes back with the canonical voice name from `dexed_banks_data.h`.

### 16.9 Hostvol round-trip

User drags Windows playback slider from 100% to 50%. Firmware polls
`AudioInputUSB_F32::volume()` next loop tick, sees the change, applies
square-law (0.25 linear), pushes to amps, broadcasts:

```
server -> /main/st/hostvol/value ,f 0.25      # to all /xremote clients
```

### 16.10 SLIP-OSC byte-level framing example

OSC message:

```
/info ,s "phase2"
```

OSC bytes (12 bytes for address + 4 for type tag + 8 for string with padding):

```
2F 69 6E 66 6F 00 00 00     # "/info\0\0\0"
2C 73 00 00                 # ",s\0\0"
70 68 61 73 65 32 00 00     # "phase2\0\0"
```

= 20 bytes total. SLIP frame:

```
C0  (FRAME_END)
2F 69 6E 66 6F 00 00 00 2C 73 00 00 70 68 61 73 65 32 00 00
C0  (FRAME_END)
```

= 22 bytes on the wire. No SLIP-ESC byte (`DB`) needed because none of the
content bytes are `C0` or `DB`.

If the payload contained a `0xC0` byte (e.g. inside a binary blob), it
would be escaped as `DB DC`, and `0xDB` would be escaped as `DB DD`, per
RFC 1055.

A bundle is the same except the address starts with `#bundle\0` and contains
a 64-bit timetag plus per-message length-prefixes.

---

## 17. Differences from X32

The mixer deviates from X32 in the following ways. Each deviation is
intentional and irreversible (changing back would break our hardware
assumptions or invent capabilities we don't have).

### 17.1 Six channels, ten reserved (vs 32)

X32 has 32 input channels plus 8 aux ins plus 8 fx returns. This board has
6 wired channels (`/ch/01..06`) with slots reserved for 10 (`/ch/07..10`
when the ADC6140 wires in). All 32 X32 leaves exist conceptually; we just
don't expose `/ch/11..32`. A client that queries them gets no reply.

### 17.2 No DCAs initially

X32 has 8 DCA groups (`/dca/[1..8]`). The mixer has zero today. The
`/ch/NN/grp/dca` leaf accepts writes (a bitmask) but they're inert. When
DCAs land (probably as a sum-mixer construct in the F32 graph), this leaf
goes live without a namespace change.

### 17.3 No matrices, no FX engine

X32 has 6 matrix outs (`/mtx/[01..06]`), 8 FX engines (`/fx/[1..8]`), and a
full FX-return-from-internal-FX path. This mixer has none of those. The
namespace is reserved.

### 17.4 Single main bus (`/main/st` only)

This hardware has one stereo output. X32 has both `/main/st` and `/main/m`
(mono main bus). Writes to `/main/m/...` are accepted-and-echoed but have
no audio effect.

### 17.5 No headamp / preamp control over network

X32 has `/headamp/[000..127]/...` for remote stagebox preamp gain. The
TAC5212's input gain is controlled through `/codec/tac5212/...` (chip-
register namespace), not exposed at `/headamp/...`. The leaf surface at
`/ch/NN/preamp/trim` is a digital trim, post-ADC, in the F32 graph.

### 17.6 `/synth/...` extension

X32 has no internal synth. We add `/synth/dexed`, `/synth/bus`, and (future)
`/synth/<engine>` branches. The internal architecture mirrors a channel
strip so an X32-aware UI can render a synth as a "channel" with the right
shape.

### 17.7 `/codec/...` extension

X32 doesn't expose its codec registers — its preamp panel is at
`/headamp/...` only. We add `/codec/tac5212` and `/codec/adc6140` for chip-
level register editing, which is necessary for board bring-up and codec
tuning. The leaf surface here is custom and documented in
`Tac5212Panel.cpp`.

### 17.8 `/-cap/hostvol/...` extension

X32 has no concept of "host volume" — it's a standalone console, not a USB-
audio device. We add the `/-cap/...` and `/main/st/hostvol/...` leaves to
expose the Windows USB Audio Class Feature Unit values.

### 17.9 `/snapshot` flat dump

X32 prefers `/node` for grouped reads. We added `/snapshot` as a one-shot
flat dump for backwards compatibility with the existing dev surface UI.
`/node` is the canonical X32 way; `/snapshot` is deprecated long-term.

### 17.10 SLIP-OSC instead of UDP

X32 uses UDP/OSC over Ethernet. This Teensy 4.1 has neither Ethernet nor
Wi-Fi out of the box, so we frame OSC with SLIP over USB CDC. The protocol
above the framing is identical; only the wire transport differs. A future
Wi-Fi build of this firmware would speak UDP/OSC on the same address tree
without changing a single leaf.

### 17.11 Rate-code engine simplified

X32 implements a 5 ms server tick with strict modulo dispatch for
`/subscribe` rate codes. The mixer today is change-driven (effectively
rate-code 1) and accepts the rate-code parameter for API compatibility but
ignores it. Full rate-code dispatch lands with the meters family.

### 17.12 No `/formatsubscribe`, `/batchsubscribe`

X32's batch-subscription primitives bundle multiple leaves into one report.
We don't implement them. Use `/node` plus `/xremote` for the equivalent
pattern.

### 17.13 `/save` and `/load` deferred

X32 saves to USB drive scenes (`name.scn` files). The mixer will eventually
save to QSPI flash slots, but the scene-format work isn't in scope for the
Phase 2 firmware. Both leaves are reserved.

---

## 18. Address index

Alphabetized table of every leaf the firmware accepts today (or is
documented as reserved). Type tags are OSC type-tag strings without the
leading comma. R/W = readable + writable, R = read-only, W = write-only,
RW* = readable, write-only echoes value (no input persisted).

| Address                          | Tag    | Range / values                       | Dir | Description                                                            |
|----------------------------------|--------|--------------------------------------|-----|------------------------------------------------------------------------|
| `/-cap/hostvol/mute`             | `i`    | 0/1                                  | R   | Windows recording-side mute (FU 0x30)                                  |
| `/-cap/hostvol/value`            | `f`    | 0..1                                 | R   | Windows recording-side slider (FU 0x30)                                |
| `/-prefs/autoLoad`               | `i`    | 0/1                                  | RW  | (reserved) load `bootSlot` at boot                                     |
| `/-prefs/bootSlot`               | `i`    | 1..N                                 | RW  | (reserved) scene slot to load at boot                                  |
| `/-prefs/midiChannel/thru`       | `i`    | 0/1                                  | RW  | (reserved) MIDI through-passing                                        |
| `/-prefs/ledBrightness`          | `f`    | 0..1                                 | RW  | (reserved) front-LED dimmer                                            |
| `/-stat/audio/blocks/f32`        | `ii`   | inUse, max                           | R   | F32 audio block-pool stats                                             |
| `/-stat/audio/blocks/i16`        | `ii`   | inUse, max                           | R   | int16 audio block-pool stats                                           |
| `/-stat/boot/reason`             | `s`    | "cold" / "softreset" / "watchdog"    | R   | last reboot reason                                                     |
| `/-stat/boot/timeMs`             | `i`    | millis                               | R   | monotonic uptime                                                       |
| `/-stat/cpu`                     | `f`    | 0..100 %                             | R   | main-loop CPU utilization                                              |
| `/-stat/fw/gitref`               | `s`    | short SHA                            | R   | git commit                                                             |
| `/-stat/fw/phase`                | `s`    | "phase2" etc.                        | R   | build phase label                                                      |
| `/-stat/fw/version`              | `s`    | semver                               | R   | firmware version                                                       |
| `/-stat/usbhost/connected`       | `i`    | 0/1                                  | R   | any USB-host MIDI device enumerated                                    |
| `/-stat/usbhost/deviceName`      | `s`    | product string                       | R   | last USB-MIDI product string                                           |
| `/arp/bpm`                       | `f`    | 20..300                              | RW  | internal-clock BPM                                                     |
| `/arp/gate`                      | `f`    | 0.05..1.0                            | RW  | gate length fraction                                                   |
| `/arp/latch`                     | `i`    | 0/1                                  | RW  | latch hold-mode                                                        |
| `/arp/octaves`                   | `i`    | 1..4                                 | RW  | octave-spread                                                          |
| `/arp/on`                        | `i`    | 0/1                                  | RW  | engage arp filter                                                      |
| `/arp/pattern`                   | `i`    | 0..4 (up/down/updown/random/asplayed)| RW  | step pattern                                                           |
| `/arp/rate`                      | `i`    | ticks-per-step                       | RW  | step rate (X32-style)                                                  |
| `/arp/swing`                     | `f`    | 0.0..0.5                             | RW  | swing depth                                                            |
| `/auxin/[01..08]/...`            | varies | mirrors `/ch/NN/...`                 | RW  | aux-in strip (reserved; not wired today)                               |
| `/ch/[01..10]/config/color`      | `i`    | 0..15                                | RW  | X32 color enum                                                         |
| `/ch/[01..10]/config/icon`       | `i`    | 0..74                                | RW  | X32 icon enum                                                          |
| `/ch/[01..10]/config/name`       | `s`    | 1..12 chars                          | RW  | user label                                                             |
| `/ch/[01..10]/config/source`     | `i`    | 0..255                               | RW  | hardware source code                                                   |
| `/ch/[01..10]/dyn/...`           | varies | (reserved)                           | RW  | per-channel comp/gate (no audio effect today)                          |
| `/ch/[01..10]/eq/N/f`            | `f`    | 20..20000 Hz                         | RW  | EQ band N center                                                       |
| `/ch/[01..10]/eq/N/g`            | `f`    | -15..+15 dB                          | RW  | EQ band N gain                                                         |
| `/ch/[01..10]/eq/N/q`            | `f`    | 0.3..10.0                            | RW  | EQ band N Q                                                            |
| `/ch/[01..10]/eq/N/type`         | `i`    | 0..5                                 | RW  | EQ band N type (LCut/LShlv/PEQ/HCut/HShlv/VEQ)                         |
| `/ch/[01..10]/eq/on`             | `i`    | 0/1                                  | RW  | EQ section engage                                                      |
| `/ch/[01..10]/grp/dca`           | `i`    | 0..255 bitmask                       | RW  | DCA membership (reserved)                                              |
| `/ch/[01..10]/grp/mute`          | `i`    | 0..63 bitmask                        | RW  | mute-group membership (reserved)                                       |
| `/ch/[01..10]/mix/fader`         | `f`    | 0..1 (X32 law)                       | RW  | channel fader                                                          |
| `/ch/[01..10]/mix/mlevel`        | `f`    | 0..1                                 | RW  | mono-bus send level (no-op)                                            |
| `/ch/[01..10]/mix/mono`          | `i`    | 0/1                                  | RW  | mono-bus send (no-op)                                                  |
| `/ch/[01..10]/mix/NN/level`      | `f`    | 0..1 (161-step law)                  | RW  | channel-to-bus send level (NN=01..16; reserved)                        |
| `/ch/[01..10]/mix/NN/on`         | `i`    | 0/1                                  | RW  | channel-to-bus send on                                                 |
| `/ch/[01..10]/mix/NN/pan`        | `f`    | -1..+1                               | RW  | channel-to-bus send pan                                                |
| `/ch/[01..10]/mix/NN/type`       | `i`    | 0..3                                 | RW  | channel-to-bus send pre/post                                           |
| `/ch/[01..10]/mix/on`            | `i`    | 0/1                                  | RW  | channel mute (1=unmuted)                                               |
| `/ch/[01..10]/mix/pan`           | `f`    | -1..+1                               | RW  | channel pan                                                            |
| `/ch/[01..10]/mix/st`            | `i`    | 0/1                                  | RW  | send to /main/st                                                       |
| `/ch/[01..10]/preamp/hpf/f`      | `f`    | 20..400 Hz                           | RW  | HPF corner                                                             |
| `/ch/[01..10]/preamp/hpf/on`     | `i`    | 0/1                                  | RW  | HPF engage                                                             |
| `/ch/[01..10]/preamp/hpf/slope`  | `i`    | 0..3                                 | RW  | HPF slope (12/18/24/36 dB)                                             |
| `/ch/[01..10]/preamp/invert`     | `i`    | 0/1                                  | RW  | phase invert                                                           |
| `/ch/[01..10]/preamp/trim`       | `f`    | -18..+60 dB                          | RW  | digital trim                                                           |
| `/clock/bpm`                     | `f`    | 20..300                              | RW  | shared musical clock BPM                                               |
| `/clock/source`                  | `i`    | 0/1 (internal/external-MIDI)         | RW  | clock source                                                           |
| `/codec/adc6140/...`             | varies | (future)                             | RW  | ADC6140 register panel                                                 |
| `/codec/tac5212/...`             | varies | per Tac5212Panel.cpp                 | RW  | TAC5212 register panel                                                 |
| `/config/auxlink/N-M`            | `i`    | 0/1                                  | RW  | aux-in pair link (reserved)                                            |
| `/config/buslink/N-M`            | `i`    | 0/1                                  | RW  | bus pair link (reserved)                                               |
| `/config/chlink/1-2`             | `i`    | 0/1                                  | RW  | ch01-ch02 stereo link                                                  |
| `/config/chlink/3-4`             | `i`    | 0/1                                  | RW  | ch03-ch04 stereo link                                                  |
| `/config/chlink/5-6`             | `i`    | 0/1                                  | RW  | ch05-ch06 stereo link                                                  |
| `/config/chlink/7-8`             | `i`    | 0/1                                  | RW  | ch07-ch08 stereo link (reserved)                                       |
| `/config/chlink/9-10`            | `i`    | 0/1                                  | RW  | ch09-ch10 stereo link (reserved)                                       |
| `/config/fxlink/1-2`             | `i`    | 0/1                                  | RW  | fxrtn pair link                                                        |
| `/config/mute/N`                 | `i`    | 0/1                                  | RW  | mute-group N master (reserved)                                         |
| `/config/mtxlink/N-M`            | `i`    | 0/1                                  | RW  | matrix pair link (reserved)                                            |
| `/config/osc/...`                | varies | (reserved)                           | RW  | test oscillator                                                        |
| `/config/routing/IN`             | varies | (reserved)                           | RW  | input patchbay                                                         |
| `/config/routing/OUT`            | varies | (reserved)                           | RW  | output patchbay                                                        |
| `/config/solo/...`               | varies | (reserved)                           | RW  | solo bus                                                               |
| `/config/talk/...`               | varies | (reserved)                           | RW  | talkback                                                               |
| `/config/tape/...`               | varies | (reserved)                           | RW  | tape return                                                            |
| `/dca/[1..8]/...`                | varies | (reserved)                           | RW  | DCA strips (no DCAs today)                                             |
| `/fx/[1..8]/...`                 | varies | (reserved)                           | RW  | FX engines (none)                                                      |
| `/fxrtn/01/config/color`         | `i`    | 0..15                                | RW  | synth-bus return color                                                 |
| `/fxrtn/01/config/name`          | `s`    | 1..12 chars                          | RW  | synth-bus return name                                                  |
| `/fxrtn/01/mix/fader`            | `f`    | 0..1 (X32 law)                       | RW  | synth-bus fader (alias of /synth/bus/mix/fader)                        |
| `/fxrtn/01/mix/on`               | `i`    | 0/1                                  | RW  | synth-bus mute (alias of /synth/bus/mix/on)                            |
| `/fxrtn/[02..08]/...`            | varies | (reserved)                           | RW  | additional fx returns                                                  |
| `/headamp/[000..127]/...`        | varies | (reserved)                           | RW  | remote stagebox preamps (none on this hardware)                        |
| `/info`                          | `ssss` | name, phase, model, version          | R   | product info                                                           |
| `/load`                          | `i`    | slot                                 | W   | (reserved) load scene from slot                                        |
| `/main/m/...`                    | varies | (reserved)                           | RW  | mono main bus (no hardware)                                            |
| `/main/st/config/color`          | `i`    | 0..15                                | RW  | main-bus color                                                         |
| `/main/st/config/name`           | `s`    | 1..12 chars                          | RW  | main-bus name                                                          |
| `/main/st/dyn/...`               | varies | (reserved)                           | RW  | master dynamics                                                        |
| `/main/st/eq/N/f`                | `f`    | 20..20000 Hz                         | RW  | master EQ band N corner (band 1 mapped to high-shelf today)            |
| `/main/st/eq/N/g`                | `f`    | -15..+15 dB                          | RW  | master EQ band N gain                                                  |
| `/main/st/eq/N/q`                | `f`    | 0.3..10.0                            | RW  | master EQ band N Q                                                     |
| `/main/st/eq/N/type`             | `i`    | 0..5                                 | RW  | master EQ band N type                                                  |
| `/main/st/eq/on`                 | `i`    | 0/1                                  | RW  | master EQ engage                                                       |
| `/main/st/hostvol/on`            | `i`    | 0/1                                  | RW  | engage hostvol stage                                                   |
| `/main/st/hostvol/value`         | `f`    | 0..1                                 | RW* | Windows playback slider read-back                                      |
| `/main/st/mix/fader`             | `f`    | 0..1 (X32 law)                       | RW  | master fader (linked mode)                                             |
| `/main/st/mix/faderL`            | `f`    | 0..1 (X32 law)                       | RW  | master fader L (unlinked mode)                                         |
| `/main/st/mix/faderR`            | `f`    | 0..1 (X32 law)                       | RW  | master fader R (unlinked mode)                                         |
| `/main/st/mix/link`              | `i`    | 0/1                                  | RW  | master L/R link                                                        |
| `/main/st/mix/on`                | `i`    | 0/1                                  | RW  | master mute                                                            |
| `/main/st/mix/pan`               | `f`    | -1..+1                               | RW  | master pan (no-op)                                                     |
| `/meters/0`                      | `b`    | (reserved)                           | R   | input meters blob (reserved)                                           |
| `/midi/cc/in`                    | `iii`  | cc, value, channel                   | W   | (reserved) UI control change                                           |
| `/midi/note`                     | `iii`  | note, vel, channel                   | R   | broadcast note event (server-out)                                      |
| `/midi/note/in`                  | `iii`  | note, vel, channel                   | W   | UI keyboard note (client-in)                                           |
| `/midi/viz/on`                   | `i`    | 0/1                                  | RW  | enable /midi/note broadcast                                            |
| `/mtx/[01..06]/...`              | varies | (reserved)                           | RW  | matrix outs                                                            |
| `/node`                          | `s`    | branch path                          | RW  | grouped read of subtree                                                |
| `/outputs/...`                   | varies | (reserved)                           | RW  | physical output routing                                                |
| `/proc/shelf/enable`             | `i`    | 0/1                                  | RW  | (deprecated) alias of /main/st/eq/on                                   |
| `/proc/shelf/freq`               | `f`    | 20..20000 Hz                         | RW  | (deprecated) alias of /main/st/eq/1/f                                  |
| `/proc/shelf/gain`               | `f`    | -24..+12 dB                          | RW  | (deprecated) alias of /main/st/eq/1/g                                  |
| `/renew`                         | `s`    | subscribed addr                      | W   | refresh /subscribe TTL                                                 |
| `/save`                          | `si`   | name, slot                           | W   | (reserved) save scene to slot                                          |
| `/snapshot`                      | -      | (no payload)                         | R   | flat dump of all user-visible state                                    |
| `/status`                        | `sss`  | state, transport, version            | R   | server status                                                          |
| `/subscribe`                     | `si`   | addr, rate code                      | W   | per-leaf subscription                                                  |
| `/synth/bus/config/color`        | `i`    | 0..15                                | RW  | synth-bus color                                                        |
| `/synth/bus/config/name`         | `s`    | 1..12 chars                          | RW  | synth-bus name                                                         |
| `/synth/bus/mix/fader`           | `f`    | 0..1 (X32 law)                       | RW  | synth-bus fader                                                        |
| `/synth/bus/mix/on`              | `i`    | 0/1                                  | RW  | synth-bus mute                                                         |
| `/synth/bus/on`                  | `i`    | 0/1                                  | RW  | (legacy) alias of /synth/bus/mix/on                                    |
| `/synth/bus/volume`              | `f`    | 0..1 (linear)                        | RW  | (legacy) alias of /synth/bus/mix/fader                                 |
| `/synth/dexed/bank/names`        | `s...` | 10 strings                           | R   | bank name list                                                         |
| `/synth/dexed/config/color`      | `i`    | 0..15                                | RW  | engine color                                                           |
| `/synth/dexed/config/name`       | `s`    | 1..12 chars                          | RW  | engine name                                                            |
| `/synth/dexed/midi/ch`           | `i`    | 0..16                                | RW  | listen channel (0=omni)                                                |
| `/synth/dexed/midi/program`      | `i`    | 0..127                               | R   | last program-change received                                           |
| `/synth/dexed/mix/fader`         | `f`    | 0..1 (X32 law)                       | RW  | engine fader                                                           |
| `/synth/dexed/mix/on`            | `i`    | 0/1                                  | RW  | engine mute                                                            |
| `/synth/dexed/on`                | `i`    | 0/1                                  | RW  | (legacy) alias of /synth/dexed/mix/on                                  |
| `/synth/dexed/voice`             | `iis`  | bank, prog, name                     | RW  | current voice (read returns name; write takes bank,prog)               |
| `/synth/dexed/voice/bank`        | `i`    | 0..9                                 | RW  | bank component of voice                                                |
| `/synth/dexed/voice/name`        | `s`    | 10 chars                             | R   | current voice name                                                     |
| `/synth/dexed/voice/program`     | `i`    | 0..31                                | RW  | program component of voice                                             |
| `/synth/dexed/voice/names`       | `is...`| bank, then 32 names                  | R   | (legacy) alias of /synth/dexed/voices/names                            |
| `/synth/dexed/voices/names`      | `is...`| bank, then 32 names                  | R   | voice name list for given bank                                         |
| `/synth/dexed/volume`            | `f`    | 0..1 (linear)                        | RW  | (legacy) alias of /synth/dexed/mix/fader                               |
| `/unsubscribe`                   | `s`    | subscribed addr                      | W   | cancel /subscribe                                                      |
| `/usb/cap/hostvol/mute`          | `i`    | 0/1                                  | R   | (legacy) alias of /-cap/hostvol/mute                                   |
| `/usb/cap/hostvol/value`         | `f`    | 0..1                                 | R   | (legacy) alias of /-cap/hostvol/value                                  |
| `/xremote`                       | -      | (no payload)                         | W   | register for push-on-change updates                                    |

### 18.1 Notes on the index

- "Reserved" leaves accept writes (state mutates) but produce no audio
  effect. They will become live in a later firmware phase without changing
  shape.
- "Legacy" aliases are preserved for the existing dev-surface UI; new
  clients should use the canonical X32-shaped path.
- Direction `RW*` (asterisk) marks leaves that are technically writable but
  whose state is overwritten on the next poll tick — `/main/st/hostvol/value`
  is the canonical case. Writing it has no lasting effect unless
  `/main/st/hostvol/on` is 0.
- Type-tag column shows the **canonical** tag the firmware emits on echo. On
  input, the boolean leaves also accept `,s "ON"/"OFF"` (section 2.2).

---

## 19. Versioning

This document version: **1.0** (initial).

Changes to the wire format (new leaves, removed leaves, changed type tags)
bump the **major or minor** of `fw/version` per semver. Dev-surface clients
are expected to read `/info` on connect and gate against the firmware version
they support.

A non-breaking addition (new optional leaves, new compatibility aliases,
new enum values) bumps **patch** only.

---

## 20. References

- Patrick-Gilles Maillot, "Unofficial X32/M32 OSC Remote Protocol", v4.06-09
  (2022-03-17). PDF available at https://x32ram.com/wp-content/uploads/download-files/X32-OSC.pdf .
  Local cache: `/c/tmp/X32-OSC.pdf` and `/c/tmp/X32-OSC.txt`.
- pmaillot/X32-Behringer C headers: `/c/tmp/x32hdr/`. Files used as
  structural reference: `X32Channel.h`, `X32Bus.h`, `X32Mtx.h`,
  `X32Auxin.h`, `X32Fxrtn.h`, `X32CfgMain.h`, `X32Output.h`, `X32Headamp.h`,
  `X32Fx.h`, `X32PrefStat.h`, `X32Misc.h`, `X32Show.h`.
- RFC 1055, "A Nonstandard for Transmission of IP Datagrams over Serial
  Lines: SLIP". https://tools.ietf.org/html/rfc1055
- OSC 1.0 specification, http://opensoundcontrol.org/spec-1_0
- Project source-of-truth firmware:
  `c:\github\t-dsp\t-dsp_software\projects\t-dsp_f32_audio_shield\src\main.cpp`
- TAC5212 register panel (codec leaf surface):
  `c:\github\t-dsp\t-dsp_software\projects\t-dsp_f32_audio_shield\src\Tac5212Panel.cpp`
