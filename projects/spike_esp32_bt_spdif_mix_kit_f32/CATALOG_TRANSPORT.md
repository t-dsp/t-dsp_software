# Catalog transport — generic file + manifest protocol

**Status:** firmware side implemented in `src/main.cpp` (2026-07-14). ESP32 relay +
web + app clients in progress. This doc is the cross-surface **contract** — build to it.

## Why

Every catalog type used to get its own bespoke wire format (`@INSTR`, `@DRUMS`, …),
re-implemented on 4 surfaces (firmware / ESP32 / web / app), each with its own
size/chunking/paging quirks. That's a 4× tax per new catalog. Instead:

- **The device is a dumb file server.** One primitive streams any SD file.
- **The client owns all semantics.** Parsing, genre/pack facets, "All" banks, paging
  — all pure client code over a parsed manifest. No firmware knowledge of the UI.
- **A new catalog type = a new file on the card + a client parser.** Zero firmware,
  ESP32, or protocol change.

## The manifest (the contract's data)

`tools/fetch_drums.py` writes `/drums/catalog.tsv`:

```
# filename<TAB>pack<TAB>genre<TAB>bpm<TAB>display
gmd funk 95bpm.mid<TAB>gmd<TAB>funk<TAB>95<TAB>gmd funk 95bpm
```

Line order = the groove's stable global index. Columns drive the two browse axes
(genre, pack), each of which gets a synthetic **"All"** bank client-side.

## Wire protocol

All frames are `@…\n` lines (survive the ESP32 UART line reader) whose payload fields
are `\x1f`-separated (unit separator; never appears in paths/base64). Requests come
from a client (web over USB CDC, or app→ESP32→Teensy over BLE); responses stream back
on the same channel.

### Requests (client → device)

| Line | Meaning |
|------|---------|
| `@READ=<path>` | Stream the file at `<path>` as base64 frames (below). |
| `@GETCAT` | (existing) re-scan SD, resend `@SONGS`/`@INSTR`/`@DRUMS`/`@MANIFESTS`. |
| `@DRUMF=<filename>` | Play the groove `/drums/<filename>` (looping). Decoupled from scan order. |
| `@DRUM=stop` | Stop the groove. (`@DRUM=<int>` is the legacy flat-menu index — avoid in new clients.) |

### File-read response frames (device → client)

| Frame | Fields |
|-------|--------|
| `@FB=<id>\x1f<path>\x1f<bytes>` | Begin: transfer id (u8, monotonic), path, total byte count. |
| `@FD=<id>\x1f<seq>\x1f<b64>` | Data: chunk `seq` (0-based), base64 of ≤360 raw bytes (→ ≤480 b64; whole `@FD` line fits one ~512 BLE MTU). |
| `@FE=<id>\x1f<count>` | End: total number of `@FD` frames sent. |
| `@FERR=<id>\x1f<reason>` | Error (e.g. `not found`). |

**Reassembly:** collect `@FD` payloads in `seq` order, **concatenate the base64
strings, then decode once** (`atob`). Raw chunk = 360 bytes (a multiple of 3), so
every non-final chunk has no `=` padding and the concatenation is valid base64. Verify
decoded length == `<bytes>`; on a gap or mismatch, re-issue `@READ` (whole-file retry).
`<id>` lets a client ignore stale frames from an aborted transfer.

### Manifest registry (device → client, part of the catalog)

`sendCatalog()` emits, re-sent on every refresh so a synth change re-points the client:

```
@MANIFESTS=<role>\x1f<source>|<role>\x1f<source>|…
```

`<source>` is one of:
- `file:<path>` — fetch via `@READ` and parse (client owns the browser).
- `bundled:<id>` — a static list the client already ships (e.g. `gm128`, `gmkits`).
- `engine` — use the `@INSTR` names already streamed.
- `none` — unavailable right now.

Current roles: `drums` (`file:/drums/catalog.tsv` or `none`), `drumkit`
(`bundled:gmkits`), `instr` (`bundled:gm128` on GM engines, else `engine`).

## Client flow (each surface, one reusable module)

1. On connect / `@GETCAT`, read `@MANIFESTS`.
2. For a role whose source is `file:<path>`, `@READ` it (later: skip if a cached copy
   matches a `@GETHASH` — not yet implemented).
3. Parse the manifest, build the browser: axis toggle (Genre / Pack) → bank picker
   (each axis has an "All" bank) → groove list. Paging is pure client code.
4. Play with `@DRUMF=<filename>` (from the manifest row); stop with `@DRUM=stop`.

## ESP32 relay responsibilities

- Relay client requests to the Teensy verbatim: `@READ=…`, `@DRUMF=…` (add BLE
  opcodes/char that emit these on `Serial7`).
- Pass the response frames (`@FB`/`@FD`/`@FE`/`@FERR`) and `@MANIFESTS` straight
  through to a NOTIFY characteristic — **no reassembly on the ESP32** (the Teensy has
  already chunked; the ESP32 just forwards each line, paced ~25 ms like the existing
  catalog notifier). This removes the old whole-value-must-fit-`line[8192]` limit.

## Firmware notes

- `streamFile(Print& out, const char* path)` in `main.cpp` does the base64 framing and
  paces `delay(6)` on any stream that isn't USB `Serial`.
- `@READ` / `@DRUMF` dispatch live in `handleControlLine`.
- Legacy `@DRUMS=` (flat 48-name list) and numeric `@DRUM=<i>` remain for now; retire
  once all clients use the manifest browser. Don't churn the Dexed `@INSTR`/`@DXLS`
  path — migrate opportunistically.
