#!/usr/bin/env python3
"""
push_ui.py -- export the tdsp-control web UI and push it onto the T-DSP's ESP32 over HTTP.

The ESP32 WiFi build (projects/t-dsp_esp32_bt_receiver, env esp32dev_wifi) raises its own
access point and serves the control UI from LittleFS. This script is how that UI gets
there -- and how it gets UPDATED afterwards: no reflash, no app store, ~250 KB over WiFi.

    python tools/push_ui.py                       # export + push to 192.168.4.1 (the AP)
    python tools/push_ui.py --host tdsp.local     # device on your LAN
    python tools/push_ui.py --no-export --dist app/tdsp-control/dist
    python tools/push_ui.py --list                # just show what the device is hosting

Token: --token, else TDSP_UI_TOKEN from projects/t-dsp_esp32_bt_receiver/.env, else
"change-me" (the firmware default). Files are gzipped here (index.html -> index.html.gz);
the firmware serves the .gz twin with Content-Encoding: gzip. Every push first clears the
old files so stale hashed bundles don't pile up in the 1.4 MB partition.

Design: planning/thin-shell-app/README.md.
"""
import argparse, gzip, io, json, os, subprocess, sys, tempfile, urllib.request, urllib.error, uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP  = os.path.join(ROOT, "app", "tdsp-control")
ENV  = os.path.join(ROOT, "projects", "t-dsp_esp32_bt_receiver", ".env")


def token_from_env():
    try:
        with open(ENV, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith("TDSP_UI_TOKEN="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return "change-me"


def export(dist):
    npx = "npx.cmd" if os.name == "nt" else "npx"
    print(f"[push_ui] expo export --platform web -> {dist}")
    subprocess.run([npx, "expo", "export", "--platform", "web", "--output-dir", dist], cwd=APP, check=True)


def http(base, path, method="GET", token=None, body=None, ctype=None, timeout=60):
    req = urllib.request.Request(base + path, data=body, method=method)
    if token: req.add_header("X-Token", token)
    if ctype: req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def multipart(devpath, data):
    b = "----tdsp" + uuid.uuid4().hex
    body = (f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{devpath}\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{b}--\r\n".encode()
    return body, f"multipart/form-data; boundary={b}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.4.1", help="device host/IP (default: the AP address)")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--token", default=None)
    ap.add_argument("--dist", default=None, help="use this export dir instead of exporting")
    ap.add_argument("--no-export", action="store_true", help="alias for --dist app/tdsp-control/dist")
    ap.add_argument("--list", action="store_true", help="only list files hosted on the device")
    ap.add_argument("--no-gzip", action="store_true")
    a = ap.parse_args()

    base = f"http://{a.host}:{a.port}"
    token = a.token or token_from_env()

    if a.list:
        st, body = http(base, "/ui/list")
        for e in json.loads(body): print(f"{e['size']:>9}  {e['name']}")
        return 0

    dist = a.dist or (os.path.join(APP, "dist") if a.no_export else None)
    tmp = None
    if not dist:
        tmp = tempfile.mkdtemp(prefix="tdsp-ui-")
        dist = tmp
        export(dist)

    files = []
    for dp, _, fns in os.walk(dist):
        for fn in fns:
            full = os.path.join(dp, fn)
            rel = "/" + os.path.relpath(full, dist).replace(os.sep, "/")
            files.append((rel, full))
    if not any(r == "/index.html" for r, _ in files):
        print(f"[push_ui] no index.html in {dist} -- not an expo web export?"); return 2

    print(f"[push_ui] clearing hosted UI on {base}")
    try:
        st, body = http(base, "/ui/clear", "POST", token, b"")
    except urllib.error.HTTPError as e:
        print(f"[push_ui] clear failed: HTTP {e.code} {e.read().decode(errors='replace')}"); return 1

    total = 0
    for rel, full in sorted(files):
        with open(full, "rb") as f: raw = f.read()
        if a.no_gzip or rel.endswith((".gz", ".png", ".ico", ".woff2")):
            data, devpath = raw, rel
        else:
            buf = io.BytesIO()
            with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as g: g.write(raw)
            data, devpath = buf.getvalue(), rel + ".gz"
        body, ctype = multipart(devpath, data)
        try:
            st, resp = http(base, "/ui", "POST", token, body, ctype, timeout=180)
        except urllib.error.HTTPError as e:
            print(f"[push_ui] {devpath}: HTTP {e.code} {e.read().decode(errors='replace')}"); return 1
        total += len(data)
        print(f"[push_ui] {devpath}  {len(raw):>8} -> {len(data):>7} B  {resp.decode(errors='replace').strip()}")

    st, body = http(base, "/ui/list")
    hosted = {e["name"]: e["size"] for e in json.loads(body)}
    missing = [r for r, _ in files if (r not in hosted and r + ".gz" not in hosted)]
    print(f"[push_ui] pushed {len(files)} files, {total/1024:.1f} KB; device hosts {len(hosted)} files")
    if missing:
        print("[push_ui] MISSING on device: " + ", ".join(missing)); return 1
    print(f"[push_ui] done -> open http://{a.host}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
