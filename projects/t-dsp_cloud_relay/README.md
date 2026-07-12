# T-DSP Cloud Relay — prototype

Authenticated **remote control of a T-DSP mixer from your phone**, from any
network, with no firewall changes. A small Railway-deployable relay sits
between a browser client (phone) and a mixer "agent" (today: the laptop
running the serial bridge; tomorrow: an on-board ESP32).

```
 phone (wss /client, login cookie) ─┐
                                     ├─► RELAY (Railway) ◄── laptop agent (wss /agent, device token) ─► serial ─► Teensy
 desktop (wss /client) ─────────────┘
```

**The relay never parses the audio protocol.** It forwards raw SLIP/OSC
bytes both directions, keyed by device id. That is what makes it
protocol-agnostic and lets the ESP32 later take the agent role unchanged —
it just opens the same `wss://…/agent` with a device token and speaks the
same bytes. The mixer connection is **outbound-only**, so there's nothing to
open on the laptop (or, later, on the customer's network).

## Pieces

| File | Runs where | Role |
|---|---|---|
| `server.mjs` | Railway | HTTP + auth + `/agent` & `/client` websockets + byte fan-out |
| `agent.mjs` | your laptop | serial ↔ outbound wss (the old bridge, dialing out) |
| `public/` | served by relay | minimal touch control surface (real OSC/SLIP) |
| `auth.mjs` | relay | tiny HMAC session tokens (prototype-grade) |
| `test/relay-loop.mjs` | local | end-to-end relay test, no hardware |

## Try it locally (no Railway, no device)

```bash
pnpm install
pnpm test:loop      # asserts auth, fan-out, presence, unsub
```

To click around the UI locally, run the relay and open it in a browser
(the agent/serial side will just show "device offline"):

```bash
AUTH_PASS=pw SESSION_SECRET=x DEVICE_TOKEN=t node server.mjs
# open http://localhost:8080  (login admin / pw)
```

## Deploy to Railway

1. Push this repo to GitHub (already is) and create a Railway project from
   it, with **root directory** `projects/t-dsp_cloud_relay`. Railway
   auto-detects Node and runs `pnpm start`.
2. Set **Variables** (see `.env.example`):
   - `AUTH_USER`, `AUTH_PASS` — your login
   - `SESSION_SECRET` — long random string
   - `DEVICE_TOKEN` — long random string (the agent must match it)
   - `DEVICE_ID` — e.g. `mixer-1`
   - `PORT` is injected by Railway; don't set it.
3. Railway gives you a public URL, e.g. `https://your-app.up.railway.app`.
   It terminates TLS, so the browser gets `https`/`wss` automatically —
   which is why a phone can connect with no certificate work.

## Run the agent on your laptop

Point the agent at the relay (note `wss://`, not `https://`):

```bash
RELAY_URL=wss://your-app.up.railway.app DEVICE_TOKEN=<same-as-relay> pnpm agent
```

It auto-detects the Teensy by USB VID/PID (or set `COM_PORT=COM4`),
dials out to the relay, and bridges serial ↔ cloud. Close the serial
monitor first (it holds the COM port).

## Control from your phone

Open `https://your-app.up.railway.app` on your phone, log in, and move a
fader. Path: phone → relay → laptop agent → serial → Teensy. The status bar
shows the link state and whether the device (agent) is online.

## This is a PROTOTYPE — what to harden before real users

- **Auth is single-user, env-configured, no password hashing.** Replace
  `auth.mjs` + the login route with real accounts (users table, hashed
  passwords, a vetted lib like `jose`) and per-user device ownership. The
  `/client` upgrade already has the seam: it currently maps every user to
  `DEVICE_ID`; swap that for "which devices does this user own?".
- **One device, in-memory registry.** `devices` is a `Map`; for a real
  fleet, back it with Postgres (device records, pairing codes) and, to run
  more than one relay instance, put a Redis/NATS pub/sub between relay nodes
  so a client on node A reaches an agent on node B.
- **Device provisioning.** Today there's one shared `DEVICE_TOKEN`. Real
  version: a unique secret/cert per mixer, issued at pairing time.
- **Billing.** Gate `/client` behind an active subscription (Stripe). Note
  the natural freemium split: local/LAN control stays free, cloud remote is
  the paid feature.

## Why a phone "app" isn't needed (yet)

The control surface is a web page served over HTTPS, so it installs to the
home screen as a PWA with no app store. Add a `manifest.webmanifest` +
service worker when you want the installed-app feel; the transport doesn't
change.
