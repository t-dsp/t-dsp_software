# OPLL (YM2413) patch banks — staged, not tracked

The `.txt` files in this folder are **fetched**, never committed (see `.gitignore`). Each file is
one bank of YM2413 *user-voice* patches (8 register bytes `$00..$07` + a name per line) and shows
up in the app as one **folder** in the OPLL synth's voice browser (Synth C on the kitchen-sink
build). The firmware loads every bank from the card's `/opll/` folder at boot
(`lib/TDspYmfm/src/OpllBank.h`); the chip's 15 ROM voices and the baked PSS-140 set are always
there too (a card bank named `PSS-140` replaces the baked copy).

## 1. Fetch

```
python tools/fetch_opll_patches.py all                 # plgDavid PortaSound rips + emu2413 ROM sets + Furnace presets
python tools/fetch_opll_patches.py plgdavid            # Yamaha PSS-140 (100), PSS-270 (~100), SHS-10 (25)
python tools/fetch_opll_patches.py emu2413             # Konami VRC7 (15) + Yamaha YMF281B (15)
python tools/fetch_opll_patches.py furnace             # Furnace tracker instruments/OPLL (.fui -> regs)
python tools/fetch_opll_patches.py vgm <pack.zip|dir|file|url> ...   # rip user voices from game VGM logs
```

For `vgm`, download packs from <https://vgmrips.net> (Sega Master System FM, MSX-MUSIC, VRC7
titles) and point the tool at the `.zip`s or a folder of them. One bank per *game* (from the GD3
tag); `--bank-per-file` makes one per track instead.

## 2. Push to the card

```
python tools/sync_assets.py --opll [--port COMx|/dev/ttyACM0]
```

or over Wi-Fi, one file at a time through the ESP32 tunnel:

```
python projects/t-dsp_esp32_bt_receiver/tools/esp32_ota_wifi.py 192.168.4.1 ^
    firmware/mix-kit/assets/opll/PSS-140.txt --stage-only --path /opll/PSS-140.txt
```

Then reboot the Teensy: banks are scanned once at boot. Check the serial log for
`[hetero-opll] SD banks: N patches in M bank(s)`.

## File format

```
# comments
#name: Display name for the folder     (optional; default = file name without .txt)
13 01 18 0F 9E 60 00 9F<TAB>Piano 1    (8 hex bytes = OPLL regs $00..$07, TAB, patch name)
```

Hand-written banks work too: drop a `.txt` in `/opll/` on the card.

## Licensing

plgDavid's rips are marked "copyrighted, please only use for study" — personal use only.
emu2413 is MIT; Furnace presets are author-credited open source; VGM rips are game data. Keep
these banks on your own boxes and don't redistribute card images containing them.
