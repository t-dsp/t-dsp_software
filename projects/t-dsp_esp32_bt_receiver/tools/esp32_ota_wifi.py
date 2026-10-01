#!/usr/bin/env python3
# esp32_ota_wifi.py -- reflash the on-board ESP32 with NO USB host, through the Teensy.
#
#   python esp32_ota_wifi.py <ws-host[:port]> <firmware.bin> [--offset 10000] [--path /esp32/firmware.bin]
#                            [--stage-only | --flash-only] [--serial /dev/ttyACM0 | COM4]
#   e.g.  python esp32_ota_wifi.py 192.168.4.1 .pio/build/esp32dev_wifi/firmware.bin
#
# How it works (the ESP32 IS the Wi-Fi link, so it can't be streamed into directly):
#   1. STAGE  : "!tunnel" turns the ESP32's WebSocket into a raw byte pipe to the Teensy's UART; through
#               it we run the Teensy's @WB file-write (header line + raw payload + CRC32) so the image
#               lands on the SD card. "!fxend" closes the pipe. @CRC re-reads the file to double check.
#   2. FLASH  : "@ESPUP=<path>\x1f<offset>" makes the Teensy reset the ESP32 into its ROM bootloader and
#               write the SD file over the UART (ROM serial protocol; firmware/mix-kit Esp32SdFlash.inc.h).
#               The WebSocket drops the moment the ESP32 is reset -- that's expected.
#   3. CONFIRM: we wait for the box to come back (AP/WS up again), read "!status" (its "fw" build stamp)
#               and "@ESPUP?" (the Teensy's result line).
#
# Needs the Teensy built with -D TDSP_ESP32_SDFLASH and the ESP32 Wi-Fi build >= 2026-10-01 ("!tunnel").
# The UART is 115200, so staging 1.7 MB takes ~2.5 min and burning it another ~2.5 min (TDSP_ESPUP_BAUD
# in the Teensy build can raise the burn speed).
#
# --serial: same protocol over the Teensy's USB serial instead of Wi-Fi (no tunnel verbs needed) -- handy
# from a Linux box plugged into the Teensy (the ESP32 reboot doesn't break USB, so no reconnect wait).
#
# Requires: pip install websocket-client   (Wi-Fi mode)
import argparse, json, os, sys, time, zlib

CHUNK = 512
US = "\x1f"


# ---- transports ----------------------------------------------------------------------------------
class WsLink:
    """WebSocket to the ESP32. TEXT frames = relayed @-lines ('\n'-terminated, may be chunked);
    BIN frames = raw tunnel bytes. Both are appended to one text buffer for pattern waits."""
    def __init__(self, url, timeout=25):
        import websocket  # websocket-client
        self.ws = websocket.create_connection(url, timeout=timeout)
        self.buf = ""
        self.ws.settimeout(0.2)

    def drain(self, t=0.2):
        self.ws.settimeout(t)
        try:
            while True:
                m = self.ws.recv()
                self.buf += m.decode("ascii", "replace") if isinstance(m, (bytes, bytearray)) else m
        except Exception:
            pass

    def send_text(self, s):   self.ws.settimeout(None); self.ws.send(s)
    def send_raw(self, b):    self.ws.settimeout(None); self.ws.send_binary(b)
    def close(self):
        try: self.ws.close()
        except Exception: pass


class SerialLink:
    """Teensy USB serial. Raw open via termios on Linux (no DTR games), pyserial elsewhere."""
    def __init__(self, port):
        self.buf = ""
        self.fd = None; self.ser = None
        if os.name == "posix":
            import termios, select
            self._select = select
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            a = termios.tcgetattr(fd)
            a[0] = 0; a[1] = 0
            a[2] = (a[2] | termios.CLOCAL | termios.CREAD) & ~termios.CRTSCTS & ~termios.HUPCL & ~termios.CSIZE | termios.CS8
            a[3] = 0; a[4] = a[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, a)
            self.fd = fd
            time.sleep(0.3); self.drain(0.3); self.buf = ""
        else:
            import serial
            self.ser = serial.Serial(port, 115200, timeout=0.05)

    def drain(self, t=0.2):
        end = time.time() + t
        while time.time() < end:
            if self.fd is not None:
                if self._select.select([self.fd], [], [], 0.05)[0]:
                    try: d = os.read(self.fd, 65536)
                    except OSError: d = b""
                    if d: self.buf += d.decode("ascii", "replace")
            else:
                d = self.ser.read(65536)
                if d: self.buf += d.decode("ascii", "replace")

    def _write(self, b):
        if self.fd is not None:
            off = 0
            while off < len(b):
                try: off += os.write(self.fd, b[off:])
                except BlockingIOError: time.sleep(0.005)
        else:
            self.ser.write(b)

    def send_text(self, s):   self._write(s.encode() + (b"" if s.endswith("\n") else b"\n"))
    def send_raw(self, b):    self._write(b)
    def close(self):
        if self.fd is not None: os.close(self.fd)
        else: self.ser.close()


def wait_for(link, needle, secs, after=0):
    """Wait until `needle` appears in link.buf past index `after`; return its index or -1."""
    end = time.time() + secs
    while time.time() < end:
        link.drain()
        i = link.buf.find(needle, after)
        if i >= 0: return i
    return -1


def line_after(link, idx):
    """The complete line starting at idx (waits briefly for its '\n')."""
    for _ in range(50):
        j = link.buf.find("\n", idx)
        if j >= 0: return link.buf[idx:j].rstrip("\r")
        link.drain(0.1)
    return link.buf[idx:]


# ---- steps ---------------------------------------------------------------------------------------
def stage(link, data, path, wifi):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if wifi:
        mark = len(link.buf)
        link.send_text("!tunnel")
        if wait_for(link, "!tunnel=on", 8, mark) < 0:
            print("  ESP32 did not open the tunnel (needs the Wi-Fi build with '!tunnel')"); return False
        time.sleep(0.2)
    mark = len(link.buf)
    hdr = f"@WB=1{US}{path}{US}{len(data)}{US}{crc:08x}\n"
    link.send_raw(hdr.encode())
    i = wait_for(link, "@WOK=1", 10, mark)
    if i < 0:
        j = link.buf.find("@WERR=", mark)
        print("  Teensy refused the write:", line_after(link, j) if j >= 0 else link.buf[mark:][-200:]); return False
    print(f"  staging {len(data)} bytes -> {path} (crc {crc:08x}) ...")
    t0 = time.time()
    for off in range(0, len(data), CHUNK):
        link.send_raw(data[off:off + CHUNK])
        if (off // CHUNK) % 256 == 0 and off:
            link.drain(0)
            if "@WERR=" in link.buf[mark:]:
                print("  ABORT:", line_after(link, link.buf.find("@WERR=", mark))); return False
            print(f"    {off // 1024}/{len(data) // 1024} KB  {off / (time.time() - t0) / 1024:.1f} KB/s")
    i = wait_for(link, "@WE=1", 30, mark)
    if i < 0:
        j = link.buf.find("@WERR=", mark)
        print("  write did not complete:", line_after(link, j) if j >= 0 else link.buf[mark:][-200:]); return False
    we = line_after(link, i)
    print(f"  {we.strip()}  ({time.time() - t0:.0f} s)")
    if f"{crc:08x}" not in we:
        print("  CRC mismatch in @WE"); return False
    if wifi:
        mark = len(link.buf)
        link.send_text("!fxend")
        wait_for(link, "!fxbridge=off", 5, mark)
        time.sleep(0.3)
    # independent re-read of the file on the card
    mark = len(link.buf)
    link.send_text(f"@CRC={path}")
    i = wait_for(link, "@CRCR=", 60, mark)
    if i < 0:
        print("  @CRC verify: no reply (continuing on the @WE crc)"); return True
    crcr = line_after(link, i)
    ok = f"{crc:08x}" in crcr and str(len(data)) in crcr
    print(f"  {crcr.strip()}  -> {'verified' if ok else 'MISMATCH'}")
    return ok


def flash(link, path, offset):
    mark = len(link.buf)
    link.send_text(f"@ESPUP={path}{US}{offset:x}")
    i = wait_for(link, "@ESPUP_", 10, mark)
    if i < 0:
        print("  no @ESPUP_GO from the Teensy (is it built with TDSP_ESP32_SDFLASH?)"); return False
    ln = line_after(link, i)
    print("  " + ln.strip())
    return ln.startswith("@ESPUP_GO=")


def confirm_wifi(url, wait_s):
    import websocket
    print(f"  waiting up to {wait_s // 60} min for the box to come back ...")
    t0 = time.time()
    while time.time() - t0 < wait_s:
        time.sleep(3)
        try:
            link = WsLink(url, timeout=5)
        except Exception:
            continue
        print(f"  back after {time.time() - t0:.0f} s")
        return confirm(link)
    print("  box did not come back"); return False


def confirm(link, wifi=True):
    if wifi:   # "!status" is answered by the ESP32 itself; over USB the Teensy never sees ESP32 verbs
        mark = len(link.buf)
        link.send_text("!status")
        i = wait_for(link, '"fw"', 8, mark)
        if i >= 0:
            j = link.buf.rfind("{", 0, i)
            try:
                st = json.loads(line_after(link, j))
                print(f"  ESP32 fw build: {st.get('fw')}")
            except Exception:
                print("  ESP32 status:", line_after(link, j))
        else:
            print("  no 'fw' in status (older ESP32 image?)")
    mark = len(link.buf)
    link.send_text("@ESPUP?")
    i = wait_for(link, "@ESPUP_LAST=", 8, mark)
    last = line_after(link, i) if i >= 0 else "(no reply)"
    print("  Teensy:", last.strip())
    return "@ESPUP_LAST=OK" in last


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="ESP32 ws host[:port] (e.g. 192.168.4.1 or tdsp.local), or '-' with --serial")
    ap.add_argument("image", help="firmware.bin to burn")
    ap.add_argument("--offset", default="10000", help="flash offset, hex (default 10000 = app)")
    ap.add_argument("--path", default="/esp32/firmware.bin", help="SD staging path")
    ap.add_argument("--stage-only", action="store_true")
    ap.add_argument("--flash-only", action="store_true", help="skip staging; burn what is already on the card")
    ap.add_argument("--serial", help="use the Teensy USB serial port instead of Wi-Fi")
    ap.add_argument("--wait", type=int, default=420, help="seconds to wait for the box after the flash (Wi-Fi)")
    a = ap.parse_args()
    offset = int(a.offset, 16)
    data = open(a.image, "rb").read()
    wifi = not a.serial
    if wifi:
        host = a.host if ":" in a.host else a.host + ":81"
        url = f"ws://{host}/"
        link = WsLink(url)
    else:
        url = None
        link = SerialLink(a.serial)
    print(f"target {a.serial or url}, image {len(data)} bytes @0x{offset:x}, SD path {a.path}")
    time.sleep(0.5); link.drain(0.5)

    if not a.flash_only:
        print("== stage to SD ==")
        if not stage(link, data, a.path, wifi): link.close(); return 2
    if a.stage_only:
        link.close(); print("staged only."); return 0

    print("== flash ESP32 from SD ==")
    if not flash(link, a.path, offset): link.close(); return 3
    if wifi:
        link.close()
        ok = confirm_wifi(url, a.wait)
    else:
        print("  (USB) watching the Teensy log ...")
        end = time.time() + 600
        while time.time() < end:
            link.drain(0.5)
            i = link.buf.rfind("[espup] ")
            if i >= 0:
                ln = line_after(link, i)
                if ln.startswith("[espup] OK") or ln.startswith("[espup] ERR"):
                    print("  " + ln.strip()); break
        # The Teensy mirrors the rebooted ESP32's boot log on USB: "[esp] [fw] built <date> <time>".
        if wait_for(link, "[esp] [fw] built", 20) >= 0:
            print("  " + line_after(link, link.buf.rfind("[esp] [fw] built")).strip())
        else:
            print("  (no ESP32 boot stamp seen on USB within 20 s)")
        time.sleep(1)
        ok = confirm(link, wifi=False)
        link.close()
    print("ESP32 UPDATE COMPLETE." if ok else "ESP32 update NOT confirmed.")
    return 0 if ok else 4


if __name__ == "__main__":
    sys.exit(main())
