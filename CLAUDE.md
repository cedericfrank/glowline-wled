# CLAUDE.md

Fork of WLED with a custom usermod (`usermods/glowline/`). This repo is **public**: everything
committed here is published. See `AGENTS.md` for WLED coding conventions.

## Session start

Follow the lumen-shared session protocol (end of this file). `docs/PLAN.md` (local-only, in
`.git/info/exclude`, never committed) holds this repo's technical design; priorities live in
`lumen-shared/ROADMAP.md`. Current state lives in `lumen-shared/status/glowline-wled.md`, never in
this public repo. Plan first.

## Rules

1. **Plan first**; stop at every GATE.
2. Stay in this repository. Exception: the attached lumen-shared directory, edited per the session
   protocol.
3. **Keys:** never open, print, copy or `cat` anything in `~/.lumen-keys/`. The signing script
   reads keys by path from `$LUMEN_OTA_KEY_DIR`; it may be run, but its output must never contain
   key material. Never commit keys or signed binaries.
4. **Never flash, erase, or push OTA to any device.** Build only; the maintainer does every device
   operation.
5. **Frozen** unless a change is justified first and approved (approved exception, 2026-10-12: the
   stock `markOTAvalid()` rollback fix in REQ-060; see DECISIONS.md): the embedded OTA public keys, the
   OTA rollback logic (including the `extern "C" verifyRollbackLater()` override), the partition
   table, the usermod's WLED usermod ID, and EUPL/WLED copyright notices.
6. One variable per release: no upstream WLED merges, platform upgrades, or refactors.
7. Verify WLED, ESP-IDF and Arduino-ESP32 specifics against source or official docs; cite file and
   line when reporting.
8. Small commits, conventional-commit messages.

## Toolchain

- PlatformIO CLI (`brew install platformio`) with `pioarduino/platform-espressif32`
  (selected per env in `platformio_override.ini`); Node.js for the web UI build.
- Target: ESP32-S3, 16MB flash + PSRAM.

## Build

```bash
npm ci && npm run build                          # web UI headers; required before any pio build
pio run -e esp32s3_lumen_prod                    # release env; esp32s3_lumen_dev for the dev Worker
```

Always pass `-e` explicitly. Firmware version is set per build via
`PLATFORMIO_BUILD_FLAGS='-D GLOWLINE_FW_VERSION=\"x.y.z\"'` (changing it wipes `.pio/build`).
Signing: `usermods/glowline/readme.md` ("Signing a release (macOS)").
Envs containing `BENCH` / `DO_NOT_SHIP`, and the `GLOWLINE_OTA_TEST_FORCE_BAD_HOST` flag, are for
bench rollback tests only; never ship their output.

## Role: on-demand
Requests in `lumen-shared/requests/` and ROADMAP.md items for the current milestone come first; `docs/PLAN.md` items stay in scope after them. "On-demand" means
no new phase starts without the maintainer's go (Rule 1). Bench stability beats features.
You own `lumen-shared/contracts/firmware.md`.

## Shared context

`~/dev/lumen-shared` must be attached to this session. If it is not (no `lumen-shared` in your
working directories), stop and ask Ceddy to restart the session with it attached.

@../lumen-shared/SESSION-PROTOCOL.md

The protocol above replaces the old "Shared context" rules (RUNBOOK.md reading, PROGRESS.md and
"For the runbook" blocks). It outranks anything else in this file about session start, reporting
and lumen-shared edits.
