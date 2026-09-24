# Glowline usermod

Bridges WLED to the Glowline backend over a TLS (`wss://`) WebSocket, applying
whatever state it's sent — turning the strip on/off, changing color, effects,
brightness, etc.

## What it does

- Connects to `wss://<host>:<port>/ws?device=<deviceId>&token=<token>&fw=<version>&caps=heartbeat,probe,ack`.
  Host, port, device ID and token are all configured at runtime via
  **Settings -> Usermods** — nothing is hardcoded or baked into the build.
  `caps` advertises the delivery-ack protocol capabilities this build
  supports (see `docs/DELIVERY-ACK.md`); the server falls back to legacy
  behavior for any capability it doesn't see listed.
- Sends a `hello` message on connect. Every inbound text frame is
  type-dispatched (`docs/DELIVERY-ACK.md` Section 3):
  - `{"type":"probe","id":...}` gets an immediate `{"type":"probe_reply","id":...}`.
  - `{"v":1,"id":...,"state":{...}}` — an acknowledged glow push — has its
    `state` unwrapped and applied via WLED's own `deserializeState()` path
    (the same one used by the JSON API), then replies
    `{"type":"state_applied","id":...}` on success or
    `{"type":"state_rejected","id":...,"reason":...}` on failure.
  - Anything else (a non-JSON reply like the server's `"pong"`, or a legacy
    raw un-enveloped state push) falls back to applying it directly to
    WLED's state, unchanged from before.
- Reconnects with exponential backoff (1s doubling up to a 60s cap) whenever
  the connection drops.
- While connected, sends both a control-frame ping and an app-level `"ping"`
  text-frame heartbeat every 30s, and requires *something* back (a pong, or
  any other inbound frame) within 90s. A TCP socket can report
  `connected() == true` while the path is actually dead; this liveness check
  is what catches that and forces a reconnect instead of sitting on a socket
  that looks fine but never delivers anything again. The app-level heartbeat
  additionally lets the server track staleness via its own hibernation
  auto-response mechanism without waking its Durable Object.
- The first time a config change (a Settings -> Usermods save, not just a
  plain reboot) leads to a successful connection, it plays an unmistakable
  "setup worked" cue: brief full-brightness green, then settles to solid
  white at 50%. Lets you confirm setup succeeded without checking a phone or
  the backend.
- Prints a heartbeat, free heap, and WebSocket state to Serial every 5
  seconds — useful when a unit is plugged into a laptop during setup.

## TLS

Every TLS connection (`/ws`, `/ota/check`, the firmware download) verifies the
server certificate, chain and hostname, against the root CA bundle ESP-IDF builds
into mbedTLS (the full Mozilla list). Nothing is pinned: Cloudflare can switch
between the CAs it issues edge certificates from. A certificate that doesn't
verify means no connection (no insecure fallback). The host must be a hostname,
not an IP address, or the hostname check fails.

## Known debt

Device provisioning (see below) is a manual D1 insert done by hand for each
unit. There's no self-registration flow yet — a device can't claim its own
ID/token on first boot. Fine for building a handful of units for known
people; needs a real claim flow (e.g. the unit shows its 6-char claim code
and calls home to redeem it) before this can ship to an unknown customer
who unboxes a unit with nobody technical present.

## Build & flash

`platformio_override.ini` at the repository root (the same directory as
`platformio.ini`) is already tracked in this repo — `git pull` gets you a
working config, no setup step needed. (It's not actually gitignored, despite
upstream WLED convention normally treating this file as personal/local-only —
worth keeping in mind before committing your own tweaks to it.) If you ever
need to recreate it from scratch, `usermods/glowline/platformio_override.ini.sample`
is the template:

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

Build and flash the S3 env — **always pin `-e` explicitly**, don't rely on
`default_envs` alone. It currently points at the S3 env too, but a bare
`pio run -t upload` (or VS Code's default Upload task, which also omits `-e`)
depending on that silently built and flashed the non-functional non-TLS env
before this was caught, and nothing stops it from pointing at the wrong env
again in the future:

```
pio run -e esp32s3_glowline_pioarduino -t upload
```

Releases are built from the Lumen envs, which extend the S3 env and only add the build-default
backend host: **`esp32s3_lumen_prod`** (ships) and `esp32s3_lumen_dev` (dev Worker). Envs whose
name contains `BENCH` / `DO_NOT_SHIP` are bench rollback tests; never ship their output.

## Signing a release (macOS)

1. Build with the release version (it is compiled in and reported as `fw=`):

   ```
   PLATFORMIO_BUILD_FLAGS='-D GLOWLINE_FW_VERSION=\"x.y.z\"' pio run -e esp32s3_lumen_prod
   ```

   Changing `PLATFORMIO_BUILD_FLAGS` makes PlatformIO wipe `.pio/build`, so copy each image out
   before the next build. Take it from `.pio/build/<env>/firmware.bin`, never from
   `build_output/release/` (WLED copies every build there under a generic name).

2. Copy it to `dist/<env>-<version>/firmware.bin` (`dist/` is gitignored; never commit signed
   binaries).

3. Sign it. The private key is read by path from `$LUMEN_OTA_KEY_DIR` (absolute path; the key file
   must be `chmod 600` or stricter); `--keyFile` is a bare filename in that directory:

   ```
   node tools/sign-release.mjs --bin dist/<env>-<version>/firmware.bin --version x.y.z --keyFile <name>.pem [--keyId primary|spare]
   ```

   The script checks the new signature against the public key embedded in `glowline.cpp` and
   writes `manifest.json` (`version`, `size`, `sha256`, `keyId`, `signature`) next to the image.
   Upload both through the backend's firmware admin page.

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
   `Lumen-XXXX`, where `XXXX` is the last 4 hex characters of its MAC
   address. Join that network from your phone.

4. **Enter home WiFi credentials.** WLED's captive portal should open
   automatically on joining the AP (or browse to `4.3.2.1` / `wled.local`
   manually). Enter the destination WiFi's SSID and password and save — the
   unit reboots and joins that network.

5. **Enter the device ID and token.** Find the unit's new IP (router client
   list, or `http://lumen-XXXXXX.local` if mDNS resolves, `XXXXXX` = last 6
   hex characters of the MAC) and open its web UI. Go to
   **Config -> Usermods**, fill in the **Lumen** section (stored under the
   `glowline` config key):
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
