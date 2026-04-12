# Handoff: `/snapshot` on-connect state dump

**Audience:** the agent working on `web_dev_surface` (the Chromium WebSerial dev
mixer at `projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/`).

**Context:** You're probably working on master (or a branch off it). The
firmware work described below is on branch
`worktree-capture-listenback-monitor` in worktree
`.claude/worktrees/capture-listenback-monitor`. You don't need to checkout
that branch — just add the client-side integration and the firmware will
answer when you flash it from that branch.

## What was built on the firmware side

The firmware's change-only broadcast pattern (both the playback hostvol path
and the new `/usb/cap/hostvol/*` capture-side path) has one failure mode: a
client that connects mid-session has no way to learn the current state. All
the signals sit at whatever default the client initialized them to — zeros,
mostly — until the user happens to move something that triggers an echo.

An earlier attempt added a 2-second periodic heartbeat. The user pushed back
and asked for an explicit "trigger" instead of periodic broadcasts. So:

**New OSC request address:** `/snapshot` (no args, no types).

When the firmware receives this message, it immediately emits a reply
bundle containing every field a fresh client needs to render the mixer
accurately. The client processes the reply through its existing inbound
dispatcher like any other echo.

## Implementation site on the firmware

`projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp`:

- `onOscMessage(msg, userData)` — intercepts `/snapshot` **before**
  passing to `g_dispatcher.route()`. If the address matches, it calls
  `broadcastSnapshot(reply)` and flushes the bundle, then returns
  without touching the dispatcher.
- `broadcastSnapshot(OSCBundle &reply)` — gathers all state.

`/snapshot` routing is deliberately **outside** `OscDispatcher` in
`lib/TDspMixer/` because the capture-side hostvol (`/usb/cap/hostvol/*`) is
sketch-local state that the dispatcher doesn't know about. Keeping the
intercept in `main.cpp` means the framework doesn't need to grow a new
extension point for one sketch-local concern.

## Wire format — request

```
/snapshot
```

- address: exactly `/snapshot`
- type tags: none (empty type tag string is fine; CNMAT/OSC sends `,` for empty)
- args: none
- SLIP-framed like any other OSC message on this transport

## Wire format — response

A single reply bundle containing the following messages (order not
significant; the client's dispatcher already handles these addresses):

```
# Per-channel state — for n in 1..kChannelCount (=6 for small-mixer MVP v1)
/ch/NN/mix/fader    f  <fader 0..1>
/ch/NN/mix/on       i  <0|1>
/ch/NN/mix/solo     i  <0|1>
/ch/NN/config/name  s  <name>

# Main bus state
/main/st/mix/faderL    f  <0..1>
/main/st/mix/faderR    f  <0..1>
/main/st/mix/link      i  <0|1>
/main/st/mix/on        i  <0|1>
/main/st/hostvol/value f  <0..1>
/main/st/hostvol/enable i <0|1>   # <- new: no broadcast helper existed, emitted inline

# Capture-side hostvol (sketch-local state — read directly from AudioOutputUSB::features)
/usb/cap/hostvol/value f  <0..1>  # Windows recording slider (FU 0x30)
/usb/cap/hostvol/mute  i  <0|1>
```

**Not included:** meter blobs (`/meters/input`, `/meters/output`,
`/meters/host`). Meters are subscription-driven and have their own separate
`/sub addSub` pathway — if the client wants meters on reconnect, call
`subscribeMeters()` the way it already does when the user toggles the
"Meters: ON" button.

**Not included:** per-channel link state (`/ch/NN/config/link`). The
dispatcher doesn't have a `broadcastChannelLink` helper and I didn't add
one for this pass — if the client needs it on reconnect, either file a
follow-up to add it server-side or have the client's `handleIncoming` treat
missing link flags as "leave as default". Currently the firmware's default
link state matches the client's default (odd channels linked), so this
only matters if the user has toggled links and then reconnected.

## Client integration — what you need to change

In `projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/`:

1. **`src/dispatcher.ts`** — add one outbound method:

   ```ts
   // Ask the firmware to dump all current state. The firmware will
   // reply with a bundle of echoes for every field; handleIncoming()
   // processes them just like any other firmware-originated update.
   // Call this on connect so a client that joined mid-session catches
   // up to the live state instead of sitting on zero-initialized signals.
   requestSnapshot(): void {
     this.sendMsg('/snapshot', '', []);
   }
   ```

   If the `sendMsg` helper doesn't accept an empty type tag string,
   use whatever form the existing code uses for no-arg OSC messages.
   Check how `sendRaw` handles the types arg — you may need to pass
   `''` or `null` depending on how CNMAT/OSC-style encoding works in
   the client. If nothing else, `sendRaw('/snapshot', '', [])` should
   work.

2. **`src/main.ts`** — in the `connect()` function, after
   `state.connected.set(true)` and before `void readLoop()`, add a
   small delay + `dispatcher.requestSnapshot()` call. The delay is to
   let the serial port fully open and the first read buffer drain
   before our first write — not strictly required but avoids racing
   the firmware's own boot chatter:

   ```ts
   // Ask firmware for a full state dump so the UI populates with
   // the current values instead of whatever the signals were
   // initialized to. Small delay lets the port settle first.
   setTimeout(() => dispatcher.requestSnapshot(), 150);
   ```

3. **No new inbound routes needed.** The snapshot reply uses addresses
   that `handleIncoming` already handles — `/ch/NN/mix/*`, `/main/st/*`,
   `/usb/cap/hostvol/*`. They'll land on the right signals automatically.

   **Exception:** if `/main/st/hostvol/enable` isn't already handled in
   `handleIncoming`, add a route for it:

   ```ts
   if (a === '/main/st/hostvol/enable' && msg.types === 'i') {
     this.state.main.hostvolEnable.set((msg.args[0] as number) !== 0);
     return;
   }
   ```

   (I believe this already exists from the earlier host-strip work, but
   double-check.)

4. **Optional — debounce repeat snapshots.** If the user disconnects and
   reconnects rapidly, `connect()` might fire a snapshot request before
   the previous bundle has finished arriving. The signals are idempotent
   (setting to the same value is a no-op) so duplicate snapshots are
   harmless, but you may want to guard against spamming the firmware.
   Not required.

## Edge cases worth knowing

- **Firmware boot vs connect timing.** If the client connects while the
  Teensy is still booting (before `setup()` finishes), the `/snapshot`
  request arrives before the OSC transport is initialized and gets
  dropped. The client should not hang — it's fire-and-forget. If this
  becomes a problem, add a "connected" event handler on the firmware side
  that broadcasts a snapshot automatically on USB CDC line state change,
  but that's a deeper change.

- **Reply size.** The bundle is small (~12 messages, ~300 bytes). Well
  under the SLIP frame budget. No chunking needed.

- **Other clients ignoring the reply.** If a second client is already
  connected, it'll also receive the snapshot reply. That's fine — the
  reply contains echoes that match what the second client already has,
  so its signals are idempotently re-set. No desync.

- **Dev surface serial console shows the traffic.** The snapshot reply
  will appear in the serial console pane as ~12 incoming OSC messages
  in rapid succession on connect. This is expected and is a useful
  debugging cue ("did my snapshot work? scroll up, check for the
  `< /ch/01/mix/fader f 1.000` line"). Don't blocklist them from the
  log; they're valuable as a "last-known state" record.

## Commits on the firmware side

On branch `worktree-capture-listenback-monitor`:

- `c8fff37f` — listenback gain stage (the actual capture monitor audio
  path, unrelated to this snapshot work but in the same branch)
- `2adae59e` — heartbeat re-broadcast (being reverted in the next commit)
- (pending) — revert heartbeat, add `/snapshot` handler + broadcast

## Why not a new TDspMixer dispatcher helper?

Keeping `/snapshot` in `main.cpp` (not `OscDispatcher`) means the framework
doesn't need a sketch-extension hook for one use case. If more sketches
grow their own local state that needs snapshotting later, the clean path
is to add a `CodecPanel`-style "SnapshotProvider" interface on the
dispatcher and let sketches register their own contributions. Not worth
it yet.

## Questions? Reach back.

If the wire format is wrong, the client integration fails, or you want a
different address (`/dump`, `/sync`, `/state/get`, etc.), tell the user
and they'll bounce it to this side. The firmware-side change is one
function + one intercept — easy to adjust.
