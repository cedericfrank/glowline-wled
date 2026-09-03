# Glowline usermod

Bridges WLED to the Glowline backend over a TLS (`wss://`) WebSocket, applying
whatever state it's sent — turning the strip on/off, changing color, effects,
brightness, etc.

## What it does

- Connects to `wss://<host>:<port>/ws?device=<deviceId>&token=<token>`. Host,
  port, device ID and token are all configured at runtime via **Settings ->
  Usermods** — nothing is hardcoded or baked into the build.
- Sends a `hello` message on connect, and applies every JSON message it
  receives directly to WLED's own state (the same `deserializeState()` path
  used by the JSON API, so a message can turn the strip on, change color,
  effects, brightness, etc).
- Reconnects with exponential backoff (1s doubling up to a 60s cap) whenever
  the connection drops.
- While connected, sends a ping every 30s and requires *something* back (a
  pong, or any other inbound frame) within 90s. A TCP socket can report
  `connected() == true` while the path is actually dead; this liveness check
  is what catches that and forces a reconnect instead of sitting on a socket
  that looks fine but never delivers anything again.
- The first time a config change (a Settings -> Usermods save, not just a
  plain reboot) leads to a successful connection, it plays an unmistakable
  "setup worked" cue: brief full-brightness green, then settles to solid
  white at 50%. Lets you confirm setup succeeded without checking a phone or
  the backend.
- Prints a heartbeat, free heap, and WebSocket state to Serial every 5
  seconds — useful when a unit is plugged into a laptop during setup.

## Known debt

TLS is via `setInsecure()` — the connection is encrypted but the server's
certificate is **not** validated (no chain-of-trust check), so this is still
vulnerable to a MITM presenting any certificate. Replace with `setCACert()`
or a pinned cert before this ships beyond hand-provisioned beta units.

Device provisioning (see below) is a manual D1 insert done by hand for each
unit. There's no self-registration flow yet — a device can't claim its own
ID/token on first boot. Fine for building a handful of units for known
people; needs a real claim flow (e.g. the unit shows its 6-char claim code
and calls home to redeem it) before this can ship to an unknown customer
who unboxes a unit with nobody technical present.

## Build & flash

Copy `platformio_override.ini.sample` to the repository root as
`platformio_override.ini` (the same directory as `platformio.ini`). That
file is gitignored, so this is a one-time local step needed to include the
usermod in a build — without it, `glowline` is not compiled in.

```
cp usermods/glowline/platformio_override.ini.sample platformio_override.ini
```

Two environments are defined:

- **`esp32s3_glowline_pioarduino`** — ESP32-S3, 16MB flash + PSRAM, built on
  the pioarduino platform, which ships a real TLS stack. **Use this one** —
  it's the only env that can actually do `wss://`.
- `esp32dev_glowline` — plain `esp32dev`. Cannot do `wss://`: the default
  tasmota-sourced espressif32 platform (and the official PlatformIO registry
  platform, which resolves to the same source) ships mbedtls with TLS
  compiled out. Kept only for quick non-TLS testing of the rest of the
  usermod.

Build and flash the S3 env:

```
pio run -e esp32s3_glowline_pioarduino -t upload
```

## Provisioning a new unit (current manual process)

This is the full process for taking a freshly flashed chip to "connected and
authenticated," as of a hand-built beta unit. There is no automated
customer-onboarding flow yet — see Known debt above.

1. **Flash the unit** per Build & flash above.

2. **Create the device record in the backend.** Before handing the unit to
   anyone, insert its row (id, token, 6-char claim code) into the `devices`
   table in the glowline-app D1 database (`glowline-app.cedericfrank.workers.dev`)
   by hand. Keep the generated device ID and token — you'll type them into
   the unit in step 5.

3. **Join the unit's setup AP.** On first boot with no WiFi configured, the
   unit broadcasts its own open (no password) access point named
   `Glowline-XXXX`, where `XXXX` is the last 4 hex characters of its MAC
   address. Join that network from your phone.

4. **Enter home WiFi credentials.** WLED's captive portal should open
   automatically on joining the AP (or browse to `4.3.2.1` / `wled.local`
   manually). Enter the destination WiFi's SSID and password and save — the
   unit reboots and joins that network.

5. **Enter the device ID and token.** Find the unit's new IP (router client
   list, or `http://wled.local` if mDNS resolves) and open its web UI. Go to
   **Config -> Usermods**, fill in the `glowline` section:
   - **Host** — the WebSocket server hostname (the glowline-app Worker)
   - **Port** — `443`
   - **Device Id** — from step 2
   - **Token** — from step 2

   Save. The unit immediately attempts a `wss://` connection using those
   values.

6. **Confirm.** On the first successful connection after that save, the
   strip flashes green then settles to solid white at 50% — that's
   confirmation the unit is connected and authenticated. No need to check
   the backend or a phone.
