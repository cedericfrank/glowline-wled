# Notice: modified version of WLED

This repository is the firmware for Lumen LED strips. It is a **modified version of
[WLED](https://github.com/wled-dev/WLED)**, which is licensed under the
[European Union Public Licence v1.2](LICENSE) (EUPL v1.2). This modified version is licensed under
the same EUPL v1.2.

- Original work: Copyright (c) 2016-present Christian Schwinne and individual WLED contributors.
- Modifications: Copyright (c) 2026 the Lumen authors.
- Based on WLED `main` as of 2026-08-24 (commit `c4564577`). Modified since 2026-08-26.
- Source code: https://github.com/cedericfrank/glowline-wled

All of WLED's copyright, licence and warranty notices are kept as they were. As the EUPL says, this
work is provided "as is", without warranty (EUPL v1.2, Articles 7 and 8).

## What was changed, and when

Every individual change, with its date and author, is in this repository's git history
(`git log`). In summary:

### WLED files that were modified

| File | Change | Dates |
| --- | --- | --- |
| `wled00/wled.h` | Setup hotspot named `Lumen-XXXX` (from the MAC) | 2026-09-03, 2026-09-22 |
| `wled00/const.h` | Default mDNS name prefix can be set per build (`lumen-XXXXXX`) | 2026-09-22 |
| `wled00/util.cpp` | Uses that mDNS name prefix for the fallback host name | 2026-09-22 |
| `wled00/wled.cpp` | mDNS name prefix; the usermod confirms a new firmware image instead of `setup()`; the setup hotspot stays up during Wi-Fi retries; the hotspot uses the board's own passphrase | 2026-09-22, 2026-10-12, 2026-10-13, 2026-10-14 |

### Files added by this fork

- `usermods/glowline/`: the Lumen usermod (cloud connection, signed firmware updates, setup hotspot).
- `usermods/ble_heap_probe/`: a bench-only measurement usermod, never shipped.
- `platformio_override.ini`: the Lumen build environments.
- `tools/sign-release.mjs` and its test: signs firmware images for release.
- `.github/workflows/lumen-ci.yml`: builds the Lumen environments on every push.
- `docs/DELIVERY-ACK.md`, `CLAUDE.md`, `NOTICE.md`, `.gitattributes`, and small edits to
  `.gitignore` and `.vscode/extensions.json`.

The workflows inherited from WLED under `.github/workflows/` are unchanged and switched off for
this repository.
