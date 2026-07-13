# T-DSP USB control page

`control.html` drives the Teensy directly over its USB serial (CDC) port using the
**Web Serial API** — the same `@`-command protocol the ESP32 relays from the BLE app,
so **no ESP32 is needed**. Plug the Teensy in over USB and go.

## Run it

Web Serial only works in a **secure context**, so open the file from `http://localhost`
(not a bare `file://`). From this folder:

```
python -m http.server 8000
```

then open <http://localhost:8000/control.html> in **Chrome or Edge** (desktop).
Firefox and Safari don't implement Web Serial.

Click **Connect**, pick the Teensy's serial port, and the page pulls the song +
instrument catalog (`@GETCAT`) and renders its pickers from whatever the firmware
reports — add a song/instrument in firmware and it shows up here, no page edit.

## Protocol (device is the source of truth)

Sent to the device (one per line, `\n`-terminated):

| Command            | Effect                                  |
|--------------------|-----------------------------------------|
| `@VOL=<0..100>`    | master volume %                         |
| `@SONG=<index>`    | start song / `@SONG=stop`               |
| `@DXVOICE=<index>` | select instrument                       |
| `@MIDIMODE=<0\|1>` | normal MIDI vs MPE                       |
| `@HPF=<mode>`      | DAC high-pass mode                      |
| `@GETCAT`          | re-scan SD + resend the catalog         |

Received from the device:

- `@SONGS=name|name|…` — song list
- `@INSTR=<0x1F>synth<TAB>desc[<TAB>GM]|inst0|inst1|…` — engine name + instruments
  (a `GM` flag means the standard 128 General-MIDI names, rendered client-side)

Any other line (heartbeat, `[esp] …`, debug) is ignored / echoed to the log.

The firmware parses these on **both** transports via the shared `handleControlLine()`
in `src/main.cpp` — USB CDC (this page) and the ESP32 UART (the BLE app).
