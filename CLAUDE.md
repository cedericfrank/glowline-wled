# CLAUDE.md

Fork of WLED with a custom usermod (`usermods/glowline/`). This repo is **public**: everything
committed here is published. See `AGENTS.md` for WLED coding conventions.

## Session start

First read the four lumen-shared files listed under "Shared context" below, then `docs/PLAN.md`
and `docs/PROGRESS.md` (local-only, listed in `.git/info/exclude`, never committed). Requests
addressed to glowline-wled come before the next unchecked item in `docs/PROGRESS.md`; RUNBOOK.md
outranks `docs/PLAN.md` where they disagree. Plan first.
Update `docs/PROGRESS.md` at every GATE and whenever the session ends (keep it under ~150 lines).

## Rules

1. **Plan first**; stop at every GATE.
2. Stay in this repository. Exception: the attached lumen-shared directory, which may be read;
   edits there are allowed only in `requests/` and `contracts/firmware.md`.
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
pio run -e esp32s3_lumen_prod                    # release env; esp32s3_lumen_dev for the dev Worker
```

Always pass `-e` explicitly. Firmware version is set per build via
`PLATFORMIO_BUILD_FLAGS='-D GLOWLINE_FW_VERSION=\"x.y.z\"'` (changing it wipes `.pio/build`).
Signing: `usermods/glowline/readme.md` ("Signing a release (macOS)").
Envs containing `BENCH` / `DO_NOT_SHIP`, and the `GLOWLINE_OTA_TEST_FORCE_BAD_HOST` flag, are for
bench rollback tests only; never ship their output.

## Role: on-demand
Requests in `lumen-shared/requests/` and RUNBOOK.md items on the current release
priority come first; `docs/PLAN.md` items stay in scope after them. "On-demand" means
no new phase starts without the maintainer's go (Rule 1). Bench stability beats features.
You own `lumen-shared/contracts/firmware.md`.

## Shared context (added 2026-09-24)

The attached lumen-shared directory is required. If it is not attached (no
`lumen-shared` in your working directories), stop and ask the maintainer to
restart the session with it attached.

At the START of every session, before `docs/PLAN.md` and `docs/PROGRESS.md`:
1. `lumen-shared/DECISIONS.md` — settled; never re-open.
2. `lumen-shared/RUNBOOK.md` — the cross-repo plan. Find the rows and items
   that name this repo. It outranks `docs/PLAN.md` where they disagree.
3. `lumen-shared/contracts/*.md` — the interfaces you build against.
4. `lumen-shared/requests/` — files addressed **To:** this repo are your
   queue. Take them before PLAN.md items unless the maintainer says otherwise.

While working:
- Edit only this repo, plus `lumen-shared/requests/` and the contract file
  this repo owns. Never edit RUNBOOK.md, DECISIONS.md, or another repo.
- Need something from another repo? Do NOT stop. Copy
  `lumen-shared/requests/_TEMPLATE.md` to `REQ-<nnn>-<slug>.md`, stub or
  mock it here, keep going.
- Verify before claiming done (build, tests, bench, curl — whatever proves it).

At every GATE and at the END of every session (unchanged from PLAN.md):
- Update `docs/PROGRESS.md`. Add a **"For the runbook"** section at the top:
  one to five lines of what changed that RUNBOOK.md should now say. The
  maintainer copies those into the runbook. PROGRESS.md is the record; at gates,
  chat gets the PROGRESS.md diff and whatever the GATE checklist in PLAN.md asks
  for, nothing more.
- Move requests you completed to `lumen-shared/requests/done/` and update
  the Status column of the contract file if an interface shipped.
- Put under **"Needs the maintainer"** only what a session cannot decide.
