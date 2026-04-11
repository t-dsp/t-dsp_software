# 05 — Vendoring Strategy

## Decision

External libraries are vendored via **`git subtree`** under `lib/`, with a manifest file (`vendored.json`) at the repo root tracking pinned upstream commits, licenses, purposes, local patches, and contribution targets. A companion tool `tools/vendor.py` provides subcommands for managing the manifest and the subtrees.

The Teensy core is vendored as a subtree at `lib/teensy_cores/`, and a Python `extra_scripts` hook overlays our `teensy4/` subdirectory onto PlatformIO's framework cache before each compile.

## Why subtree (not submodule, not lib_deps)

Three options were considered for getting external libraries into the project:

### Option A: PlatformIO `lib_deps` URLs

```ini
[env:teensy41]
lib_deps =
    https://github.com/h4yn0nnym0u5e/OSCAudio.git
    https://github.com/chipaudette/OpenAudio_ArduinoLibrary.git
```

**Rejected.** Pulls libraries at build time into `.pio/libdeps/`, which is not in the repo. Network-dependent builds, no version pinning by default, no easy way to apply local patches, no clean way to contribute back.

### Option B: Git submodules

```bash
git submodule add https://github.com/h4yn0nnym0u5e/OSCAudio.git lib/OSCAudio
```

**Rejected.** Submodules are pointer files, not actual code in the working tree. Downstream users need to remember `git clone --recurse-submodules`. Updates are awkward (`git submodule update --remote`). Local patches are *technically* possible but require committing inside the submodule with all the friction that implies. PlatformIO's library dependency finder (LDF) sometimes struggles with submodule directories.

### Option C: Git subtree (chosen)

```bash
git subtree add --prefix=lib/OSCAudio oscaudio master --squash
```

**Chosen.** Subtree merges the upstream repo's history into our repo as a subdirectory of real files. Properties:

- **Files live in our repo as actual files.** PlatformIO LDF sees them naturally. Downstream users get everything with a normal `git clone`. No nested git operations.
- **`git subtree pull`** brings upstream updates into our subdirectory as a single squash commit (with `--squash`).
- **`git subtree push`** to a fork remote pushes our local changes to a branch on the fork, so we can open PRs back to upstream without leaving the working tree.
- **Local patches are normal commits** in our repo's history. They're visible in `git log`, easy to find, easy to revert, easy to roll forward when upstream changes.
- **Single working tree.** No `--recurse-submodules`, no `git submodule init`.

The cost of subtree is that the merge commits look slightly funny (`Squashed 'lib/OSCAudio/' content from commit abc1234`), and you need to remember which prefix corresponds to which upstream when running pull/push commands. The `vendored.json` manifest solves the second problem.

## The `vendored.json` manifest

A JSON file at the repo root that records, for each vendored library:

- Where it came from (remote URL, branch)
- What version is currently in the subtree (pinned commit hash)
- When and why it was added
- What license it ships under
- What local patches we've applied on top of the pinned commit
- What contributions we plan to send back upstream

### Format

```json
{
  "$schema": "./tools/vendored.schema.json",
  "vendoredLibraries": {
    "OSCAudio": {
      "prefix": "lib/OSCAudio",
      "remote": "https://github.com/h4yn0nnym0u5e/OSCAudio.git",
      "branch": "master",
      "pinnedCommit": "<filled in by subtree add>",
      "addedDate": "2026-04-11",
      "license": "MIT",
      "purpose": "OSC dispatch helpers, OSCSubscribe machinery, /teensy*/audio/ debug surface",
      "localPatches": [],
      "upstreamFork": "https://github.com/jayshoe/OSCAudio.git",
      "contributionTargets": [
        "F32 wrapper extensions for OpenAudio classes",
        "SLIP-over-USB-CDC + multiplexed-debug-text example"
      ]
    },
    "OpenAudio_ArduinoLibrary": {
      "prefix": "lib/OpenAudio_ArduinoLibrary",
      "remote": "https://github.com/chipaudette/OpenAudio_ArduinoLibrary.git",
      "branch": "master",
      "pinnedCommit": "<filled in>",
      "addedDate": "2026-04-11",
      "license": "MIT",
      "purpose": "F32 audio path for the entire mixer signal graph",
      "localPatches": [],
      "upstreamFork": null,
      "contributionTargets": []
    },
    "Audio": {
      "prefix": "lib/Audio",
      "remote": "https://github.com/PaulStoffregen/Audio.git",
      "branch": "master",
      "pinnedCommit": "<filled in>",
      "addedDate": "2026-04-11",
      "license": "MIT",
      "purpose": "Stock Teensy Audio Library; vendored to allow pinning to specific commits and applying uncommitted PRs (multi-channel USB)",
      "localPatches": [],
      "upstreamFork": null,
      "contributionTargets": [
        "Multi-channel USB audio extensions to AudioInputUSB / AudioOutputUSB (after Spike 2)"
      ]
    },
    "teensy_cores": {
      "prefix": "lib/teensy_cores",
      "remote": "https://github.com/PaulStoffregen/cores.git",
      "branch": "master",
      "pinnedCommit": "<filled in>",
      "addedDate": "2026-04-11",
      "license": "MIT",
      "purpose": "Teensy 4.x core; vendored to enable modifications to USB descriptors, USB audio buffers, and feature units. Overlaid onto PlatformIO framework cache via tools/cores_overlay.py",
      "localPatches": [],
      "upstreamFork": null,
      "contributionTargets": [
        "Multi-channel USB audio support",
        "USB Audio Class feature unit on input terminal (Windows recording slider)"
      ]
    }
  }
}
```

`pinnedCommit` is the commit hash from the upstream repo that's currently checked out in our subtree. It's updated whenever we pull from upstream or apply a patch. CI verifies that the working tree matches the manifest.

## The `tools/vendor.py` tool

Subcommands for managing the manifest and the subtrees:

| Command | Purpose |
|---|---|
| `vendor.py status` | Show pinned commit vs current subtree state for each entry. Flag any drift. |
| `vendor.py verify` | CI-friendly check; exit non-zero if state ≠ manifest. |
| `vendor.py update <name>` | Run `git subtree pull --prefix=<prefix> <remote> <branch> --squash`. Update `pinnedCommit` to the new HEAD. |
| `vendor.py update <name> <sha>` | Pin to a specific commit (e.g. a PR head). Run subtree pull against that ref. |
| `vendor.py add <name> <url> <branch> --prefix=lib/<name>` | Add a new vendored library. Run `git subtree add`. Append to manifest. |
| `vendor.py freeze` | One-shot bootstrap: record current subtree HEADs into manifest. |
| `vendor.py contribute <name> <branch>` | Push subtree to a fork remote on a feature branch (for opening PRs upstream). |

The `update <name> <sha>` form is the key one for "uncommitted PR" use cases. When there's an open PR against `PaulStoffregen/Audio` that we need (multi-channel USB), we point the manifest at the PR head SHA, run `vendor.py update Audio <sha>`, and the subtree pulls that exact state. The manifest records *why* we pinned to a non-master commit so future-us isn't confused.

The `localPatches` array tracks any further modifications we've made on top of the pinned upstream commit, with descriptions and ideally a diff or patch file path. This is what lets us carry temporary local fixes without losing track of them.

## Workflow examples

### Adding a new vendored library

```bash
git remote add -f mylib https://github.com/someone/mylib.git
python tools/vendor.py add mylib https://github.com/someone/mylib.git master --prefix=lib/mylib
# vendor.py runs: git subtree add --prefix=lib/mylib mylib master --squash
# vendor.py captures the resulting commit SHA into vendored.json
# vendor.py prompts for purpose, license, contribution targets
git add vendored.json
git commit -m "vendor: add mylib"
```

### Updating to upstream HEAD

```bash
python tools/vendor.py update OSCAudio
# vendor.py runs: git subtree pull --prefix=lib/OSCAudio oscaudio master --squash
# vendor.py updates pinnedCommit in vendored.json
git add vendored.json
git commit -m "vendor: update OSCAudio to <sha>"
```

### Pinning to a PR commit

```bash
# A PR against PaulStoffregen/Audio adds multi-channel USB. PR head is at sha abc1234.
python tools/vendor.py update Audio abc1234
# vendor.py runs: git subtree pull --prefix=lib/Audio teensyaudio abc1234 --squash
# vendor.py updates pinnedCommit and adds a note to localPatches
```

### Contributing a fix back upstream

```bash
# We added a fix to lib/OSCAudio/OSCAudioBase.cpp in our repo.
git add lib/OSCAudio/OSCAudioBase.cpp
git commit -m "OSCAudio: fix XYZ"

# Push the subtree to our fork's branch
python tools/vendor.py contribute OSCAudio fix-xyz
# vendor.py runs: git subtree push --prefix=lib/OSCAudio oscaudio-fork fix-xyz

# Then open a PR from jayshoe/OSCAudio:fix-xyz to h4yn0nnym0u5e/OSCAudio:master
gh pr create --repo h4yn0nnym0u5e/OSCAudio --base master --head jayshoe:fix-xyz \
    --title "Fix XYZ" --body "..."
```

### Verifying CI matches manifest

```bash
python tools/vendor.py verify
# Exit 0 if all subtrees match their pinned commits in vendored.json
# Exit 1 with a diff if there's drift (someone forgot to update the manifest)
```

## The Teensy core overlay mechanism

The Teensy 4.x core lives in PlatformIO's package cache at:

```
~/.platformio/packages/framework-arduinoteensy/cores/teensy4/
```

PlatformIO downloads it as part of the Teensy framework package. We don't want to vendor the entire `framework-arduinoteensy` package (huge — toolchain configs, all bundled libraries, board defs), only the parts we modify.

### The overlay script

Our `tools/cores_overlay.py` runs as a PlatformIO `extra_scripts` hook before each compile. It does:

1. Locate PlatformIO's framework cache (`platform_packages` reports the path).
2. Compute a stable identifier for the framework version currently in cache (commit hash or version string).
3. Compare against a baseline recorded in `vendored.json` for `teensy_cores`. If the framework version has changed unexpectedly, warn (the overlay may not be correct against the new version).
4. Copy `lib/teensy_cores/teensy4/` over `framework-arduinoteensy/cores/teensy4/`. Whole-file replacement, not a patch.
5. Mark the overlay as applied (so subsequent rapid rebuilds skip the copy).

### Why whole-file overlay (not patches)

A patch hunk references specific line numbers and surrounding context in the upstream file. When the upstream file changes (PlatformIO updates the framework), patches may fail to apply, and the failure mode is silent corruption or compile errors that look unrelated to vendoring.

Whole-file overlay is robust. Our `lib/teensy_cores/teensy4/usb_audio.cpp` *is* the file we want compiled; whatever was at the upstream path before is replaced. If the upstream version drifted enough that our overlay is incorrect, we discover it at compile time in our own source — not as a mysterious patch-application failure.

The cost: when we want to incorporate upstream improvements, we have to manually merge them into our vendored copy (via subtree pull + conflict resolution). That's exactly the right tradeoff for a low-frequency-update target like the Teensy core.

### `lib/Audio/` is separate from cores overlay

The Audio library lives in `framework-arduinoteensy/libraries/Audio/`, not in `framework-arduinoteensy/cores/`. Vendoring the cores subtree does not address it. We vendor `lib/Audio/` separately as its own subtree and use the `lib_ignore = Audio` mechanism to make PlatformIO use our copy.

There's a small redundancy here (two separate vendoring approaches for two related upstream repos) but it matches the actual upstream structure: cores and Audio are separate repos maintained by Paul Stoffregen, and PlatformIO's framework package combines them via its own packaging step. We mirror that separation.

## Initial subtree commands

The bootstrap commands to run once when setting up the vendored libraries (the actual execution happens in Spike 1):

```bash
# Add upstream remotes (lives in .git/config, not in working tree)
git remote add -f oscaudio   https://github.com/h4yn0nnym0u5e/OSCAudio.git
git remote add -f openaudio  https://github.com/chipaudette/OpenAudio_ArduinoLibrary.git
git remote add -f teensyaudio https://github.com/PaulStoffregen/Audio.git
git remote add -f teensycores https://github.com/PaulStoffregen/cores.git

# Add the subtrees
git subtree add --prefix=lib/OSCAudio                  oscaudio   master --squash
git subtree add --prefix=lib/OpenAudio_ArduinoLibrary  openaudio  master --squash
git subtree add --prefix=lib/Audio                     teensyaudio master --squash
git subtree add --prefix=lib/teensy_cores              teensycores master --squash

# Capture pinned commits into vendored.json
python tools/vendor.py freeze
```

Each `subtree add` produces a merge commit; the second-parent commit hash of each is the upstream HEAD at the time of vendoring, which is what `vendor.py freeze` records as `pinnedCommit`.

## What this strategy doesn't address

- **Transitive dependencies.** OpenAudio depends on the stock Audio library, and OSCAudio depends on CNMAT/OSC. The manifest tracks each direct vendoring but doesn't model the dependency graph between them. If OpenAudio bumps its expected Audio library version, we have to notice and update both. For four libraries this is manageable manually; if the dep graph grows we'd want a real package manager.
- **Build reproducibility across machines.** Two developers cloning the repo at the same commit will get identical vendored library content (because subtree commits are content-addressable). But the *toolchain* (PlatformIO version, compiler version, host OS) is not pinned by `vendored.json`. That's a separate concern, addressed by `platformio.ini` constraints.
- **CI for vendor updates.** When we run `vendor.py update`, we should also run the spike test suite to confirm nothing broke. That's a CI workflow item, not a manifest concern.
