# CLAUDE.md

Fork of WLED with a custom usermod (`usermods/glowline/`). This repo is **public**: everything
committed here is published. See `AGENTS.md` for WLED coding conventions.

## Session start

Read `docs/PLAN.md` and `docs/PROGRESS.md` (local-only, listed in `.git/info/exclude`, never
committed). Continue from the next unchecked item in `docs/PROGRESS.md`. Plan first.
Update `docs/PROGRESS.md` at every GATE and whenever the session ends (keep it under ~150 lines).

## Rules

1. **Plan first**; stop at every GATE.
2. Stay in this repository.
3. **Keys:** never open, print, copy or `cat` anything in `~/.lumen-keys/`. The signing script
   reads keys by path from `$LUMEN_OTA_KEY_DIR`; it may be run, but its output must never contain
   key material. Never commit keys or signed binaries.
4. **Never flash, erase, or push OTA to any device.** Build only; the maintainer does every device
   operation.
5. **Frozen** unless a change is justified first and approved: the embedded OTA public keys, the
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
pio run -e esp32s3_glowline_pioarduino           # always pass -e explicitly
```

Firmware version is set per build: `-D GLOWLINE_FW_VERSION=\"x.y.z\"`.
Envs containing `BENCH` / `DO_NOT_SHIP`, and the `GLOWLINE_OTA_TEST_FORCE_BAD_HOST` flag, are for
bench rollback tests only; never ship their output.
