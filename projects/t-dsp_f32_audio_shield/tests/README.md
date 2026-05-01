# T-DSP F32 audio shield -- OSC + audio test harness

Python pytest suite that exercises the firmware over its USB CDC serial
endpoint (SLIP-framed OSC) and over its USB Audio endpoint (loopback
THD/SNR). The whole point of this harness is so the X32-style OSC tree
redesign can be validated end-to-end every time we change something --
treat the tests as the executable spec for the protocol.

The firmware OSC redesign hasn't landed yet. **Tests will fail today;
that's expected.** They serve as a target for the firmware work.

## Layout

```
tests/
  conftest.py                    # pytest fixtures (port discovery, OSC client)
  pytest.ini                     # markers + rootdir config
  osc_client.py                  # SLIP-OSC client + X32 fader law (no test logic)
  test_osc_basics.py             # discovery, read/write/echo
  test_fader_law.py              # 4-segment dB law (round-trip + cardinals)
  test_stereo_link.py            # /config/chlink/N-M mirroring
  test_volume_responsiveness.py  # OSC -> audio latency (the killer regression test)
  test_node_group_read.py        # /node packed-text bulk read
  test_subscribe.py              # /xremote and /subscribe push semantics
  test_audio_loopback.py         # THD / SNR / crosstalk via WASAPI loopback
  README.md                      # this file
```

`osc_client.py` is plain Python. Import it standalone if you want to
script against the firmware without pytest.

## Install

```bash
pip install pytest pyserial numpy scipy sounddevice
```

`numpy + scipy + sounddevice` are only needed for the audio tests. If
you only care about OSC, `pytest + pyserial` is enough -- the audio
tests are auto-skipped when sounddevice isn't importable.

## COM port discovery

The harness auto-detects the Teensy by USB VID (0x16C0) + Teensy PIDs
(0x0483 = USB Serial, 0x0489 = MIDI+Audio+Serial, 0x048A = Audio+Serial,
0x048B = MIDI+Serial). To list what's currently enumerated:

```bash
python -c "import serial.tools.list_ports as p; [print(x.device, hex(x.vid or 0), hex(x.pid or 0), x.description) for x in p.comports()]"
```

Override auto-detect:

```bash
# Windows:
set TDSP_TEST_PORT=COM7
# Linux/macOS:
export TDSP_TEST_PORT=/dev/ttyACM0
```

If no Teensy is found, every test marked `hardware` is skipped with a
clear reason. The pure-math fader-law tests still run.

## Audio loopback wiring

For `test_audio_loopback.py` and `test_loopback_level_follows_fader`:

* Cable from codec **OUT1** -> codec **IN1+** (single-ended).
* Cable from codec **OUT2** -> codec **IN2+**.
* Tie the unused IN1- / IN2- to ground (the codec datasheet's standard
  single-ended hookup).
* Set the Windows "Teensy Audio" output volume slider to 100% -- the
  firmware exposes that slider as a multiplier; non-100% just
  attenuates the test signal and skews the THD floor.
* No exclusive-mode required; tests use WASAPI shared mode at 48 kHz.

Override the audio device name hint (default "teensy"):

```bash
set TDSP_TEST_AUDIO_HINT=alex
```

## Running the suite

Run the whole thing from the firmware project root:

```bash
cd projects/t-dsp_f32_audio_shield
pytest tests/
```

Or pick a single file:

```bash
pytest tests/test_osc_basics.py -v
```

Or a single test:

```bash
pytest tests/test_fader_law.py::test_fader_to_db_cardinals -v
```

Skip the slow tests (xremote TTL, audio sweep):

```bash
pytest tests/ -m "not slow"
```

Run only OSC tests, no audio:

```bash
pytest tests/ -m "not audio"
```

Run only pure-math tests (no hardware needed at all):

```bash
pytest tests/ -m "not hardware"
```

Show print output (useful for the latency / loopback tests, which
print measured numbers):

```bash
pytest tests/ -v -s
```

## Markers

| Marker     | Meaning                                                  |
| ---------- | -------------------------------------------------------- |
| `hardware` | Requires the Teensy enumerated as a CDC serial port      |
| `audio`    | Additionally requires WASAPI Teensy Audio + loopback cable |
| `slow`     | Takes >5 s (xremote TTL renew, sweep tests)              |

Tests without any marker are pure math (fader law) and run unconditionally.

## Coexistence with the dev surface bridge

The dev surface (`projects/t-dsp_web_dev/serial-bridge.mjs`)
also opens the Teensy COM port. **Stop the bridge before running the
test harness** (Ctrl+C in the terminal where you started it, or close
the dev surface tab if you spawned it from there). The COM port is
single-writer; both sides will fight if both are open.

After the tests finish, restart the bridge if you want the dev surface
back.

## Style notes

* SLIP framing is RFC 1055 (END=0xC0, ESC=0xDB, ESC_END=0xDC,
  ESC_ESC=0xDD). The firmware demuxes OSC vs. plain-text CLI by frame
  start byte 0xC0.
* Booleans go on the wire as integer `,i 0` or `,i 1` -- not OSC
  `,T`/`,F`. The encoder converts Python bools automatically.
* All addresses are lowercase X32-convention (`/ch/01/mix/fader`,
  `/main/st/mix/on`, `/config/chlink/1-2`).
* Fader law is the X32 4-segment piecewise linear (PDF appendix p.
  132); cardinal points are baked into `test_fader_law.py`. The
  firmware MUST agree on the grid (1024 steps); tests round-trip
  cardinals through the firmware to verify.

## What to do when a test fails

1. **Latency tests** (`test_fader_write_to_echo_latency`,
   `test_continuous_drag_no_queue_buildup`, `test_loopback_level_follows_fader`):
   the user-reported "volume bars moving slowly" bug. Check that the
   firmware's OSC dispatcher runs in the audio update tick, not in
   `loop()` after a long `delay()`.

2. **Quantization drift** (`test_fader_quantization`,
   `test_round_trip_through_firmware`): the firmware's internal step
   count differs from 1024. Update `fader_quantize` in `osc_client.py`
   only if the new step count is intentional.

3. **Echo missing** (`test_echo_on_xremote`, `test_xremote_push`):
   `/xremote` not implemented or subscription table wired wrong.

4. **Chlink mirror failures**: the link table either doesn't exist or
   isn't fanning writes. See `test_stereo_link.py` docstrings for the
   exact X32 contract.

5. **Audio metric regressions**: probably a hardware change (different
   loopback cable, different USB volume slider position) before
   suspecting firmware. Re-run the standalone `loopback_test.py` in
   `projects/teensy_usb_audio_tac5212/` for a sanity check.
