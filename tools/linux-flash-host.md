# Linux flashing host (jay-mint) — setup & runbook

A spare Linux box on the LAN, driven **over SSH from the Windows dev machine**, that
builds and flashes both MCUs on the `teensy41_digital_audio_board`:

- **Teensy 4.1** — over USB (`teensy_loader_cli` / HalfKay).
- **ESP32-DevKitC** — over the Teensy **serial bridge** (no separate ESP32 USB).

Reference box: `jay-mint.local`, user `jay`. **Linux Mint 20 (Ulyana)** — an Ubuntu-20.04
base, so **glibc 2.31 and Python 3.8**. Those old versions are the source of most of the
one-time fixes below; a newer distro needs fewer of them.

> All edits described here were made to the box's *own copy* of the repo (streamed to
> `~/t-dsp_software`), never to the Windows source. Port only the ones marked **UPSTREAM**
> back into the tree.

---

## 0. SSH access (non-interactive, from Windows Git Bash)

Git Bash has no `sshpass`/`plink`, and the flashing session runs headless, so drive `ssh`
with an **`SSH_ASKPASS` helper** and force it with `SSH_ASKPASS_REQUIRE=force`:

Put the box's login password in the `JAYMINT_PW` env var (`export JAYMINT_PW=...` in your
shell, or a git-ignored `.env` you `source`) — **never commit it.** Better still, drop
password auth entirely: `ssh-copy-id jay@jay-mint.local` and delete the askpass dance below.

```bash
# one-time: a helper that echoes the password from the env
printf '#!/bin/sh\necho "$JAYMINT_PW"\n' > askpass.sh && chmod +x askpass.sh

SSH() { SSH_ASKPASS=./askpass.sh SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh -o StrictHostKeyChecking=accept-new -o PreferredAuthentications=password \
            -o PubkeyAuthentication=no jay@jay-mint.local "$@"; }
```

Because askpass supplies the password, **stdin is free** — you can pipe data to the remote
(`cat tree.tgz | SSH 'tar xzf - -C ~/dst'`) or feed a heredoc to a remote `cat > file`.

`sudo` works non-interactively by piping the password: `echo "$JAYMINT_PW" | sudo -S -p "" <cmd>`
(user is in `sudo`). Don't combine a heredoc-to-`bash -s` with `echo "$JAYMINT_PW" | sudo -S` — the
two fight over stdin. Write the script to a file first, then `echo "$JAYMINT_PW" | sudo -S bash file`.

**Getting the repo onto the box:** tar-stream the working tree — a `git clone` would miss
uncommitted WIP. Include `lib/`, `tools/`, `vendored.json` (repo-root marker used by
`cores_overlay.py`), and the project dir(s):

```bash
tar czf tree.tgz --exclude=.git --exclude=.pio --exclude='*.sf2' --exclude='*.wav' \
    lib tools vendored.json projects/spike_esp32_bt_spdif_mix_kit_f32 projects/t-dsp_esp32_bt_receiver
cat tree.tgz | SSH 'rm -rf ~/t-dsp_software && mkdir ~/t-dsp_software && tar xzf - -C ~/t-dsp_software'
```

---

## 1. Base install

```bash
echo "$JAYMINT_PW" | sudo -S -p "" env DEBIAN_FRONTEND=noninteractive apt-get install -y git python3-venv
# NOTE: apt-get update may fail on stale 3rd-party repos (chrome/mopidy). Run the install
# standalone (cached indexes are fine for these core packages); don't gate it on `&&`.

# PlatformIO Core into an isolated venv, then expose on PATH
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py               # -> ~/.platformio/penv
echo "$JAYMINT_PW" | sudo -S -p "" bash -c '
  ln -sf /home/jay/.platformio/penv/bin/pio        /usr/local/bin/pio
  ln -sf /home/jay/.platformio/penv/bin/platformio /usr/local/bin/platformio
  usermod -aG dialout jay                     # serial access (new logins pick it up)
  curl -fsSL https://www.pjrc.com/teensy/00-teensy.rules -o /etc/udev/rules.d/00-teensy.rules
  udevadm control --reload-rules && udevadm trigger'
```

---

## 2. Old-distro compatibility fixes (Mint 20 / glibc 2.31 / Py 3.8)

The Teensy build breaks in three places on this box. Windows (Py 3.13, teensy@5.1.0 cached)
never hits them.

1. **`tools/cores_overlay.py` — Python 3.8 + `.exe` assumptions.**
   - Add `from __future__ import annotations` at the top (the script uses PEP 604
     `X | None` annotations, which evaluate at def-time and throw on 3.8). **UPSTREAM**
     (harmless portability fix).
   - Its `env.Replace(CC=".../gcc.exe", …)` block hard-codes Windows `.exe`. Make it
     OS-conditional: `exe = ".exe" if os.name == "nt" else ""` then `f"{prefix}gcc{exe}"`.
     **UPSTREAM**.

2. **Pin `platform = teensy@5.1.0`** in the project `platformio.ini`. `platform = teensy`
   (unpinned) pulls the latest (5.2.0), which adds the linker flag
   `--no-warn-rwx-segments`; the pinned gcc-11 `ld` doesn't recognize it and linking fails.
   5.1.0 is what Windows already has cached. **Box-only** (don't change the shared ini
   upstream unless Windows also moves to 5.2.0).

3. **`teensy_size` / `teensy_post_compile` need GLIBC 2.33/2.34** (box has 2.31), so
   `pio run` fails at the size-check and `pio run -t upload` fails in the GUI uploader.
   - Shim `~/.platformio/packages/tool-teensy/teensy_size` (back up as `.orig`) with a
     `/bin/sh` wrapper that runs `arm-none-eabi-size "$elf"` and `exit 0`.
   - **Do NOT use `pio run -t upload`.** `teensy_loader_cli` in the same package *does*
     run on glibc 2.31 — flash with it directly (§3). **Box-only.**

---

## 3. Flashing the Teensy

```bash
cd ~/t-dsp_software/projects/spike_esp32_bt_spdif_mix_kit_f32
pio run -e teensy41_dexed_pool                      # build only
TL=~/.platformio/packages/tool-teensy/teensy_loader_cli
$TL --mcu=TEENSY41 -w -v .pio/build/teensy41_dexed_pool/firmware.hex
```

- **Get into program mode (HalfKay):**
  - If the running firmware is alive: send the TDspProgrammingKit `U` command over the USB
    serial (`printf 'U' > /dev/ttyACM0`) — no button.
  - If it's hung/crash-looping: **physical PROGRAM button** is the only way (can't be done
    remotely; the ESP32 could pulse the pin, but only if *its* firmware is running).
- **Quirk:** on this box the *first* `teensy_loader_cli` attempt after entering HalfKay
  often prints `error writing to Teensy`. The device stays in HalfKay (not bricked) — just
  **kill the loader and run a fresh one**; the second attempt programs cleanly.
- `teensy_loader_cli -s` (soft reboot) can't reach an MTP-mode device — hence the `U`
  command or the button.

---

## 4. Flashing the ESP32 (over the Teensy bridge)

No direct ESP32 USB — it's flashed through the Teensy, which must be running the mix-kit
firmware (TDspProgrammingKit). Needs the **pin37→ESP32-EN jumper** (present on the ref board).

```bash
cd ~/t-dsp_software/projects/t-dsp_esp32_bt_receiver
pio run -e esp32dev                                 # -> bootloader/partitions/firmware .bin

PORT=/dev/ttyACM0
stty -F $PORT 115200 clocal -crtscts -hupcl raw -echo
printf 'g' > $PORT           # Teensy: reset ESP32 into ROM download + USB<->ESP32 passthrough
sleep 3
PY=~/.platformio/penv/bin/python
ET=~/.platformio/packages/tool-esptoolpy/esptool.py
B=.pio/build/esp32dev
BA=~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
# ALWAYS --before/--after no_reset and --baud 115200 (Teensy already did the reset; the
# bridge UART is fixed-rate). Sanity-check first with `... flash_id` (no write).
$PY $ET --chip esp32 --port $PORT --baud 115200 --before no_reset --after no_reset write_flash -z \
  0x1000 $B/bootloader.bin 0x8000 $B/partitions.bin 0xe000 $BA 0x10000 $B/firmware.bin
printf '@BOOTAPP@' > $PORT    # exit flash mode -> Teensy soft-reboot -> boots ESP32 app
```

~1.2 MB at 115200 over the bridge takes ~65 s (esptool compresses with `-z`). The first
espressif32 platform build downloads the whole toolchain+framework (~8 min).

---

## 5. Desktop auto-prober gotchas (a logged-in GUI session is running)

The mix-kit's `USB_MTPDISK_SERIAL` USB type makes the Teensy a composite **MTP + Serial**
device, and the Cinnamon desktop's auto-mounters keep grabbing and **USB-resetting** it
(device re-enumerates every ~8 s, killing serial and flashing). Neutralize all three:

```bash
echo "$JAYMINT_PW" | sudo -S -p "" bash -c '
  systemctl stop ModemManager; systemctl mask ModemManager       # grabs /dev/ttyACM* as a "modem"
  D=/usr/share/dbus-1/services; M=/usr/share/gvfs/remote-volume-monitors
  mv $D/org.gtk.vfs.GPhoto2VolumeMonitor.service{,.disabled}      # gphoto2/mtp probers keep
  mv $D/org.gtk.vfs.MTPVolumeMonitor.service{,.disabled}          #   USB-resetting the device
  mv $M/gphoto2.monitor{,.disabled}; mv $M/mtp.monitor{,.disabled}'
kill $(pgrep -f "photo2-volume-mon[i]tor") $(pgrep -f "gvfs-mtp-volume-mon[i]tor")
# udev ignore rule (see /etc/udev/rules.d/99-teensy-no-automount.rules):
#   ATTRS{idVendor}=="16c0", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{MTP_NO_PROBE}="1",
#     ENV{ID_MTP_DEVICE}="0", ENV{ID_GPHOTO2}="0", ENV{UDISKS_IGNORE}="1"
```

- **`pgrep -f "…monitor…"` self-matches the SSH command line** (which contains that string)
  and kills its own shell → `ssh` exits 255. Use the bracket trick: `mon[i]tor`. Same for
  any `pkill -f` whose pattern appears in your own command (e.g. a `teensy_loader_cli` path).
- Best long-term fix for a Linux flash host: **build the firmware as plain `USB_SERIAL`**
  (no MTP) so the device is a stable `16c0:0483` CDC device. See §6.

---

## 6. Reading the Teensy serial on Linux

`cdc_acm` reports `failed to set dtr/rts` for this device, and a plain open / pyserial gets
**0 bytes**. Open with `clocal` (ignore modem-control lines) instead:

```bash
# robust reader that survives re-enumeration
timeout 20 bash -c 'while true; do
  [ -e /dev/ttyACM0 ] && { stty -F /dev/ttyACM0 115200 clocal -crtscts raw -echo 2>/dev/null; cat /dev/ttyACM0; }
done'
```

The mix-kit firmware only prints its boot banner + `CrashReport` once per boot and is
otherwise quiet until polled, so you'll only catch the banner if the reader is attached
during the first ~1.5 s after enumeration (it blocks on `while(!Serial)`).

---

## 7. BLE end-to-end test from the Windows machine

To verify the ESP32→app path without the phone, use this PC's Bluetooth as a GATT client
(`bleak`). Windows BT is often off — power it on via WinRT first:

```powershell
# PowerShell: turn the Bluetooth radio On (Windows.Devices.Radios)
# RequestAccessAsync -> GetRadiosAsync -> $bt.SetStateAsync('On')
```
```python
# python -m pip install bleak ; then scan for name/UUID/MAC-prefix C4:DD:57,
# connect, start_notify on songs(7a9c0005)+instr(7a9c0006), write REFRESH_CAT(0x23) to
# CMD(7a9c0002), reassemble "<seq>\x1e<count>\x1e<payload>" chunks.  Service 7a9c0001.
```
The ESP32 allows **one BLE central** — disconnect the phone app first or it won't advertise.

---

## Hardware notes

- Two physical boards seen in this work: **jay-mint board SN 7681380** (8 MB PSRAM, has an
  SD card) and the **local Windows board SN 18402920** (COM4, no PSRAM/SD).
- ESP32 identity: A2DP + BLE both advertise as **"T-DSP"**; BLE MAC `C4:DD:57:CA:B4:CA`.
- See also `lib/TDspProgrammingKit/README.md` (the `g`/`U`/`r`/`@BOOTAPP@` protocol) and the
  memory note `project_jaymint_flash_host.md`.
