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
        self.ws = websocket.create_connection(url, timeout=25)
        self.buf = ""

    def drain(self, t=0.3):
        # Use a SHORT timeout only for reads, then restore blocking mode so that
        # send() waits out TCP backpressure (the ESP paces us to 115200 via a full
        # UART TX FIFO) instead of raising a socket timeout mid-transfer -- the bug
        # that stalled the first attempt and hung the Teensy.
        self.ws.settimeout(t)
        try:
            while True:
                m = self.ws.recv()
                self.buf += m.decode("ascii", "replace") if isinstance(m, bytes) else m
        except Exception:
            pass
        finally:
            self.ws.settimeout(None)   # blocking sends for the streaming phase

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


MAX_ATTEMPTS = 5


def connect_retry(url, tries=30, delay=3):
    # After a failed flash the Teensy reboots, which resets the ESP -> it drops off
    # WiFi for a few seconds. Keep trying until the WS server is back.
    for k in range(tries):
        try:
            return Tunnel(url)
        except Exception as e:
            if k == 0:
                print(f"  waiting for ESP32 WS to come back ({e})...")
            time.sleep(delay)
    return None


def flash_once(url, data, attempt):
    print(f"\n=== attempt {attempt}/{MAX_ATTEMPTS} ===")
    t = connect_retry(url)
    if not t:
        print("  could not reach ESP32 WS"); return False
    time.sleep(0.5)
    t.drain()
    t.ws.send("!fxflash")
    if t.wait("reading hex lines", 10) is None:
        print("  didn't see 'reading hex lines' -- streaming anyway")

    print(f"  streaming {len(data)} bytes ({CHUNK}-byte frames)...")
    t.ws.settimeout(None)   # blocking sends: TCP backpressure paces us to the UART
    t0 = time.time()
    for i in range(0, len(data), CHUNK):
        try:
            t.ws.send_binary(data[i:i + CHUNK])
        except Exception as e:
            print(f"  send failed at {i}: {e}"); return False
        if (i // CHUNK) % 256 == 0:
            t.drain()
            if "abort -" in t.buf:                     # bad line / timeout is now FATAL
                print(f"  ABORT after {i} bytes:", t.buf[t.buf.find('abort'):][:80]); return False
            if i:
                print(f"    {i//1024}/{len(data)//1024} KB  {i/(time.time()-t0)/1024:.1f} KB/s")
    print(f"  streamed in {time.time()-t0:.1f}s; waiting for confirm...")

    m = t.wait(r"enter (\d+) to flash", 40, regex=True)
    if not m:
        if "abort -" in t.buf:
            print("  ABORT:", t.buf[t.buf.find('abort'):][:80])
        else:
            print("  no confirm prompt:", t.buf[-160:])
        return False
    n = m.group(1)
    print(f"  image validated ({n} lines) -> confirming flash")
    t.ws.send_binary((n + "\n").encode())
    if t.wait("flash_move", 12):
        print("  flash_move() issued -- Teensy writing flash + rebooting. SUCCESS.")
        time.sleep(2)
        try: t.ws.close()
        except Exception: pass
        return True
    print("  no flash_move seen:", t.buf[-160:])
    return False


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: fxflash_wifi.py <host[:port]> <firmware.hex>")
    host = sys.argv[1]
    if ":" not in host:
        host += ":81"
    url = f"ws://{host}/"
    data = open(sys.argv[2], "rb").read()
    print(f"target {url}, image {len(data)} bytes")

    for attempt in range(1, MAX_ATTEMPTS + 1):
        if flash_once(url, data, attempt):
            print("\nOVER-WIFI FLASH COMPLETE.")
            return 0
        if attempt < MAX_ATTEMPTS:
            print("  attempt failed -- waiting ~20s for Teensy+ESP to reboot, then retrying...")
            time.sleep(20)
    print("\nFAILED after all attempts.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
