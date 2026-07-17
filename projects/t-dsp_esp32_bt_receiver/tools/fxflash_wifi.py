#!/usr/bin/env python3
# fxflash_wifi.py -- reflash the Teensy OVER WiFi, through the ESP32 FlasherX tunnel.
#
#   python fxflash_wifi.py <ws-host[:port]> <firmware.hex>
#   e.g.  python fxflash_wifi.py tdsp.local:81 firmware.hex
#
# Path: this PC --WebSocket--> ESP32 --UART0--> Teensy Serial7 --> FlasherX (@FXUP).
# The ESP32 firmware (projects/t-dsp_esp32_bt_receiver, WiFi build) turns into a
# transparent byte tunnel on the "!fxflash" text command; we then stream the Intel-hex
# as binary frames and complete FlasherX's "enter N to flash" confirm handshake.
#
# The ESP<->Teensy UART is fixed 115200, so a full mix-kit image (~1.7 MB of ASCII hex)
# takes ~2-3 min. That's the price of 115200 over a real UART (the USB path is faster
# only because USB-CDC ignores the baud). Raising both UART ends would cut this.
#
# Requires: pip install websocket-client
import sys, time, re

try:
    import websocket  # websocket-client (NOT the asyncio 'websockets' package)
except ImportError:
    sys.exit("need: pip install websocket-client")

CHUNK = 512   # BIN frame size; matches the ESP's WS buffer headroom


class Tunnel:
    def __init__(self, url):
        self.ws = websocket.create_connection(url, timeout=10)
        self.ws.settimeout(0.2)
        self.buf = ""

    def drain(self):
        try:
            while True:
                m = self.ws.recv()
                self.buf += m.decode("ascii", "replace") if isinstance(m, bytes) else m
        except Exception:
            pass

    def wait(self, pattern, secs, regex=False):
        end = time.time() + secs
        while time.time() < end:
            self.drain()
            if regex:
                m = re.search(pattern, self.buf)
                if m:
                    return m
            elif pattern in self.buf:
                return self.buf
            time.sleep(0.1)
        return None


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: fxflash_wifi.py <host[:port]> <firmware.hex>")
    host = sys.argv[1]
    if ":" not in host:
        host += ":81"
    url = f"ws://{host}/"
    data = open(sys.argv[2], "rb").read()

    print(f"connecting {url} ...")
    t = Tunnel(url)
    time.sleep(0.5)
    t.drain()  # discard the initial status line

    print("requesting flash tunnel (!fxflash)...")
    t.ws.send("!fxflash")
    if t.wait("reading hex lines", 8) is None:
        print("  warning: didn't see 'reading hex lines' -- streaming anyway")

    print(f"streaming {len(data)} bytes as {CHUNK}-byte frames...")
    t0 = time.time()
    for i in range(0, len(data), CHUNK):
        t.ws.send_binary(data[i:i + CHUNK])
        if (i // CHUNK) % 128 == 0:
            t.drain()  # keep the socket healthy; check for early abort
            if "abort -" in t.buf:
                print("STREAM ABORT:", t.buf[-160:]); t.ws.send("!fxend"); return 3
            if i:
                print(f"  {i}/{len(data)}  {i/(time.time()-t0)/1024:.1f} KB/s")
    print(f"streamed in {time.time()-t0:.1f}s; waiting for confirm prompt...")

    m = t.wait(r"enter (\d+) to flash", 40, regex=True)
    if not m:
        print("NO confirm prompt. tail:", t.buf[-200:]); t.ws.send("!fxend"); return 4
    n = m.group(1)
    print(f"device validated image, wants {n} -> confirming flash")
    t.ws.send_binary((n + "\n").encode())

    if t.wait("flash_move", 10):
        print("flash_move() issued -- Teensy is writing flash and will reboot.")
    else:
        print("no flash_move seen; tail:", t.buf[-200:])
    time.sleep(2)
    try:
        t.ws.send("!fxend"); t.ws.close()
    except Exception:
        pass
    print("done. (ESP32 tunnel also drops on UART idle after the Teensy reboots.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
