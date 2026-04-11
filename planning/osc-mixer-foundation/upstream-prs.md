# Upstream PR / Issue Prep

Materials prepared so Jay can open PRs against upstream repos with minimal additional work. Each entry has: target repo, severity, reproduction, proposed fix (file/line), suggested PR title and body, and any caveats.

**Open these in the order listed.** The OSCAudio bug fix is highest priority because it's blocking T-DSP's spike validation right now.

When opening a PR, also update `vendored.json`:
- Add the PR URL to the relevant `localPatches[].upstreamPr` field
- Add the fork remote URL to the library's `upstreamFork` field
- After the PR is merged upstream, remove the entry from `localPatches` and bump `pinnedCommit`

---

## PR 1 — OSCAudio: NULL deref in `staticPrepareReplyResult` for empty reply bundles

**Repo:** `h4yn0nnym0u5e/OSCAudio`
**Type:** Real bug fix (memory corruption / undefined behavior)
**Severity:** High — silently wedges callers that don't pre-load the reply bundle
**Hardware reproduction:** Spike 1 on Teensy 4.1, 2026-04-11 (Jay's bench)

### The bug

`OSCUtils::staticPrepareReplyResult` in `lib/OSCAudio/OSCUtils.cpp:101-118` unconditionally indexes the reply bundle's last message:

```cpp
OSCMessage& OSCUtils::staticPrepareReplyResult(OSCMessage& msg, OSCBundle& reply)
{
    int msgCount = reply.size();                              // 0 when bundle is empty
    OSCMessage* pLastMsg = reply.getOSCMessage(msgCount-1);   // <-- getOSCMessage(-1)
    int dataCount = pLastMsg->size();                         // <-- NULL/garbage deref
    size_t addrL = getMessageAddressLen(msg);
    char* replyAddress = getMessageAddress(*pLastMsg, alloca(addrL), addrL);
    char* buf = getMessageAddress(msg, alloca(addrL), addrL);

    if (0 != dataCount)
        pLastMsg = &reply.add(replyAddress);

    pLastMsg->add(buf);
    return *pLastMsg;
}
```

When `reply.size() == 0`, `msgCount - 1 == -1`. CNMAT/OSC's `OSCBundle::getOSCMessage(int)` does not bounds-check; it returns either NULL or an out-of-bounds pointer. The subsequent `pLastMsg->size()` then either:

- Segfaults on hosted platforms (where the OS catches NULL deref)
- Returns garbage on Teensy 4.x (no MMU to fault on wild pointers)
- The garbage `dataCount` then drives an `alloca()` of arbitrary size and a `getMessageAddress()` call on the same wild pointer, eventually wedging the loop

### Reproduction

Minimal Teensy 4.1 sketch using stock OSCAudio:

```cpp
#include <OSCBundle.h>
#include <OSCMessage.h>
#include "OSCAudioBase.h"

OSCAudioAmplifier amp("amp");
OSCBundle replyBundle;  // empty

void setup() {
    Serial.begin(115200);
    while (!Serial) {}
    delay(1000);

    // Build a /teensy1/audio/amp/g f 0.5 message in memory
    OSCMessage msg("/teensy1/audio/amp/g");
    msg.add(0.5f);

    Serial.println("about to call routeAll on empty bundle");
    OSCAudioBase::routeAll(msg, 14, replyBundle);  // wedges here
    Serial.println("never reached");
}

void loop() {}
```

Symptom: the second `Serial.println` never fires; main loop is wedged.

In T-DSP's spike, the same wedge is reached via `msg.route("/teensy*/audio", routeOscAudioPassthrough)` followed by `OSCAudioBase::routeAll(msg, addrOff, replyBundle)`. Diagnostic prints confirmed control reaches the call but never returns.

### The undocumented assumption

OSCAudio's README says (under OSCSubscribe → "OSCBundle format for results"):

> "The current implementation pre-loads the OSCBundle passed to OSCSubscription::update() with a zero timestamp and a single OSCMessage with its address pattern pre-set."

So the implicit invariant is that **reply bundles passed to `routeAll` always contain at least one message**. The subscription update flow satisfies this; direct callers don't.

This invariant is not enforced and not documented in `staticPrepareReplyResult` itself — only mentioned in the README in the context of `OSCSubscribe`. Any caller using `routeAll` directly without going through subscriptions hits the bug.

### Proposed fix

Two options. Option A is minimal-change; Option B is more defensive.

**Option A (preferred): guard the index, treat empty bundle the same as "first reply".**

```cpp
OSCMessage& OSCUtils::staticPrepareReplyResult(OSCMessage& msg, OSCBundle& reply)
{
    int msgCount = reply.size();
    OSCMessage* pLastMsg = nullptr;
    int dataCount = 0;
    if (msgCount > 0) {
        pLastMsg = reply.getOSCMessage(msgCount - 1);
        dataCount = pLastMsg->size();
    }

    size_t addrL = getMessageAddressLen(msg);
    char* replyAddress = (msgCount > 0)
        ? getMessageAddress(*pLastMsg, alloca(addrL), addrL)
        : (char*)"/reply";  // synthetic address for first message in empty bundle
    char* buf = getMessageAddress(msg, alloca(addrL), addrL);

    if (msgCount == 0 || dataCount != 0)
        pLastMsg = &reply.add(replyAddress);

    pLastMsg->add(buf);
    return *pLastMsg;
}
```

**Option B (also fine): add an assert + always preload internally.**

```cpp
OSCMessage& OSCUtils::staticPrepareReplyResult(OSCMessage& msg, OSCBundle& reply)
{
    if (reply.size() == 0) {
        reply.add("/reply");  // synthesize the missing placeholder
    }
    // ... existing code unchanged ...
}
```

Option B is fewer lines but mutates caller-visible state in a non-obvious way. Option A is more change but preserves the original intent.

### Suggested PR title

`Fix NULL deref in staticPrepareReplyResult when reply bundle is empty`

### Suggested PR body

```
## Summary

`OSCUtils::staticPrepareReplyResult` unconditionally indexes the reply
bundle's last message via `reply.getOSCMessage(reply.size() - 1)`. When
the bundle is empty (size == 0), this passes -1 to `getOSCMessage`,
which returns NULL or an out-of-bounds pointer. The subsequent
`pLastMsg->size()` and `getMessageAddress(*pLastMsg, ...)` calls then
dereference the bad pointer and the function either crashes (on hosted
platforms) or wedges the calling loop (on Teensy 4.x with no MMU).

## Reproduction

Minimal Teensy 4.1 sketch attached in [link]. Calling
`OSCAudioBase::routeAll(msg, 0, emptyReplyBundle)` from a sketch that
hasn't pre-loaded the reply bundle reliably wedges the main loop on
the first dispatched message.

The implicit invariant — that reply bundles always contain at least
one message before being passed to `routeAll` — is documented in the
README (under OSCSubscribe → "OSCBundle format for results") but is
not enforced or noted in `staticPrepareReplyResult` itself. Callers
using OSCAudio's dispatch directly without going through OSCSubscribe
hit the bug.

## Fix

Guard the `getOSCMessage(msgCount - 1)` access with `if (msgCount > 0)`
and treat the empty-bundle case as equivalent to the "first reply"
case. The function still adds a new message via `reply.add()` when
needed, but no longer touches a NULL/garbage pointer first.

## Discovered by

T-DSP project (forum.pjrc.com user JayShoe), April 2026, while
building an OSC mixer control surface foundation that uses OSCAudio's
debug surface (`/teensy*/audio/<obj>/<method>`) directly without
involving OSCSubscribe.
```

### Caveats

- The bug only fires when calling `routeAll` (or any path that routes through `staticPrepareReplyResult`) on an empty bundle. Existing OSCAudio examples don't trigger it because they go through the subscription flow which preloads.
- Fixing it shouldn't break any existing behavior since the empty-bundle case is currently UB.
- T-DSP currently works around the bug by preloading the reply bundle in its passthrough function. The workaround can be removed once the fix is merged and pinned.

---

## PR 2 — OSCAudio: document the "preload reply bundle" invariant

**Repo:** `h4yn0nnym0u5e/OSCAudio`
**Type:** Documentation
**Severity:** Low — but ships alongside PR 1 to make the invariant visible

If PR 1 lands as Option A (which preserves original intent), the docs should also be updated so direct callers know to preload. If PR 1 lands as Option B (which preloads internally), this PR is unnecessary.

### Suggested change

Add a paragraph to `OSCAudioBase.h` above the `routeAll` declaration:

```cpp
/**
 * Route a message for the audio system to every known object.
 *
 * NOTE: The reply OSCBundle MUST contain at least one OSCMessage when
 * passed in. OSCAudio's reply machinery (staticPrepareReplyResult) assumes
 * this and indexes the bundle's last message. Callers using OSCSubscribe
 * get this preload for free; direct callers should add a placeholder
 * message (e.g. reply.add("/reply")) before calling routeAll.
 */
static void routeAll(OSCMessage& msg, int addressOffset, OSCBundle& reply);
```

And the same note in the README under "Internal routing → /audio".

### Caveats

- Skip this PR if PR 1 lands as Option B.

---

## PR 3 — OpenAudio_ArduinoLibrary: add `setPeakingEq` to `AudioFilterBiquad_F32`

**Repo:** `chipaudette/OpenAudio_ArduinoLibrary`
**Type:** Feature addition
**Severity:** Low (no bug; missing convenience method)
**Author:** The other chat

### What

Add `setPeakingEq(uint32_t stage, float frequency, float gain, float q)` to `AudioFilterBiquad_F32.h`. The implementation is already in place in T-DSP's vendored copy at `lib/OpenAudio_ArduinoLibrary/AudioFilterBiquad_F32.h:225`.

The patch follows RBJ's Audio EQ Cookbook formulas, parameter shape mirrors `setLowShelf(stage, frequency, gain, slope)` exactly except `q` instead of `slope`, and it completes the cookbook convenience set alongside the existing `setLowpass / setHighpass / setBandpass / setNotch / setLowShelf / setHighShelf` methods.

### Why

`AudioFilterBiquad_F32` already implements every other filter type from the RBJ cookbook except peaking EQ. Peaking EQ is the most common parametric EQ band in pro channel strips (T-DSP needs it for `/ch/NN/eq/B/type "peq"`), and adding it brings the F32 library to convenience-parity with the I16 `AudioFilterBiquad`. Math has been validated against musicdsp.org's RBJ cookbook reference, including the unity case (gain = 0 dB → H(z) = 1 at all frequencies).

### Suggested PR title

`Add setPeakingEq to AudioFilterBiquad_F32`

### Suggested PR body

```
## Summary

Adds `setPeakingEq(stage, frequency, gain, q)` to AudioFilterBiquad_F32.
The implementation uses the RBJ Audio EQ Cookbook formulas
(musicdsp.org/files/Audio-EQ-Cookbook.txt, peakingEQ section) and
follows the parameter convention of the existing setLowShelf method.

## Why

`AudioFilterBiquad_F32` implements every other RBJ cookbook filter type
already (`setLowpass / setHighpass / setBandpass / setNotch / setLowShelf
/ setHighShelf`). Peaking EQ — the most common parametric band type in
professional channel strips — is the missing piece. Without it, callers
have to hand-compute biquad coefficients and use `setCoefficients()`
directly, which defeats the purpose of the cookbook helpers.

## Math notes

The unity case (gain = 0 dB, A = 1) reduces to H(z) = 1 at all
frequencies — verified by symbolic substitution into the difference
equation. CMSIS sign convention applied to a1, a2 to match the existing
methods in the file.

## Discovered by

T-DSP project, while building a 4-band parametric EQ for an OSC mixer
control surface.
```

### Caveats

- Source comment in the patch currently includes a "Pending upstream PR" / "Tracked in vendored.json" line that should be **stripped before opening the PR**. The actual change to commit is just the method body and a brief RBJ-cookbook reference comment.
- T-DSP carries this as a local patch. Once merged and a new pinned commit is pulled, remove the entry from `vendored.json` `OpenAudio_ArduinoLibrary.localPatches`.
- Optional bonus: Chip's repo has no top-level `LICENSE` file — the MIT terms are only in source headers and GitHub's SPDX detector returns 404. A trivial second PR adding `LICENSE` would be a good-citizen contribution but is unrelated to T-DSP's use.

---

## PR 4 — PaulStoffregen/cores: add `<type_traits>` include to `IntervalTimer.h`

**Repo:** `PaulStoffregen/cores`
**Type:** Real bug fix (upstream completeness — file fails to compile in isolation)
**Severity:** Low (only manifests when `IntervalTimer.cpp` is compiled in a TU that doesn't transitively pull in `<type_traits>`)
**Author:** T-DSP (me) — discovered while building Spike 1 with PlatformIO

### The bug

`cores/teensy4/IntervalTimer.h` (current upstream HEAD on master) uses `std::is_arithmetic_v`, `std::is_integral_v`, `std::is_floating_point_v`, and `if constexpr` — all C++17 features defined in `<type_traits>` — but does not `#include <type_traits>` itself.

The file appears to compile in normal Teensyduino builds because something else in the include chain (probably one of the `Arduino.h` umbrella headers) transitively pulls `<type_traits>` in first. PlatformIO's standalone compile of `IntervalTimer.cpp` for `framework-arduinoteensy 1.153.0` doesn't get that lucky transitive include and fails with:

```
error: 'is_arithmetic_v' is not a member of 'std'
error: expected '(' before 'constexpr'
```

### Reproduction

PlatformIO project with `framework-arduinoteensy 1.153.0` and a newer toolchain (`platformio/toolchain-gccarmnoneeabi-teensy 1.110301.0` / gcc 11.3.1). Build any sketch — `IntervalTimer.cpp` fails to compile.

T-DSP hits this in Spike 1 because we vendor `cores/teensy4/` and overlay it onto the framework cache. Without the local patch, no Teensy 4.x build succeeds in our PlatformIO setup.

### Proposed fix

One-line addition to the existing `#include` block in `cores/teensy4/IntervalTimer.h`:

```diff
 #include <stddef.h>
+#include <type_traits>
 #include "imxrt.h"
 #if TEENSYDUINO >= 159
 #include "inplace_function.h"
 #endif
```

### Suggested PR title

`Include <type_traits> in IntervalTimer.h`

### Suggested PR body

```
## Summary

`cores/teensy4/IntervalTimer.h` uses `std::is_arithmetic_v`,
`std::is_integral_v`, `std::is_floating_point_v`, and `if constexpr`
in `IntervalTimer::cyclesFromPeriod()`, but does not include
`<type_traits>`. The file currently compiles only because some other
header in the typical Arduino include chain pulls type_traits in
transitively.

## Reproduction

PlatformIO project targeting Teensy 4.1 with `framework-arduinoteensy
1.153.0` and a newer toolchain (`toolchain-gccarmnoneeabi-teensy
1.110301.0`, gcc 11.3.1) fails to compile `IntervalTimer.cpp`:

    error: 'is_arithmetic_v' is not a member of 'std'

The standalone compile of `IntervalTimer.cpp` doesn't get the lucky
transitive include that the Arduino.h-driven build chain provides.

## Fix

One-line `#include <type_traits>` addition to the existing include
block at the top of the header.

## Discovered by

T-DSP project (forum.pjrc.com user JayShoe), April 2026, while building
on PlatformIO with a vendored copy of cores/teensy4/.
```

### Caveats

- The fix has zero risk: it adds a header that was already being pulled in indirectly. The standard library include is idempotent.
- Once merged and a new pinned commit is pulled, T-DSP can remove the patch from `vendored.json` `teensy_cores.localPatches`.
- A separate, larger issue (not part of this PR) is the **toolchain mismatch in `platform-teensy 5.1.0`**: it ships `framework-arduinoteensy 1.153.0` (which uses C++17 features like `std::align_val_t`) but bundles `toolchain-gccarmnoneeabi 5.4.1` (which doesn't support those features). This is a `platform-teensy` packaging issue, not a `cores` issue, but worth raising with Paul as awareness when this PR is opened.

---

## Summary table

| # | Repo | Type | Lines changed | T-DSP localPatches entry |
|---|---|---|---|---|
| 1 | h4yn0nnym0u5e/OSCAudio | Bug fix | ~5 | None yet — workaround in `routeOscAudioPassthrough` only |
| 2 | h4yn0nnym0u5e/OSCAudio | Doc | ~10 | None |
| 3 | chipaudette/OpenAudio_ArduinoLibrary | Feature | ~15 | `OpenAudio_ArduinoLibrary.localPatches[0]` |
| 4 | PaulStoffregen/cores | Bug fix | 1 | `teensy_cores.localPatches[0]` |

**Recommended order:** PR 1 → PR 2 → PR 4 → PR 3.

PR 1 first because it's blocking and high-severity. PR 2 immediately after PR 1 (or combined with it) because it documents the same invariant. PR 4 next because it's a one-line trivial fix that costs nothing to send. PR 3 last because it's a feature addition rather than a bug fix and doesn't have the same urgency.

## When to send

**Wait until Spike 1 is fully green** before sending PR 1. The PR description's credibility hinges on "found by hardware test, fix verified by re-test." A pre-verified PR is much weaker than one that says "applied this fix, here's the spike that runs cleanly with it."

The other three PRs can be sent at any time.
