# ble-setup: set up a board's Wi-Fi over Bluetooth (bench tool)

Does from a Mac what the Lumen app does over Bluetooth: finds a board, checks its device ID and sends
the Wi-Fi settings, encrypted with a key made from the board's hotspot passphrase. Use it to test a
board before the app supports Bluetooth setup. The protocol is described in
`usermods/glowline/glowline_ble.cpp`.

A board runs Bluetooth only while its setup hotspot is up (no Wi-Fi saved, or the saved Wi-Fi out of
reach), or for 5 minutes after BOOT is held 5-10 s. Once it is online, Bluetooth is off until the
next restart.

## One-time setup

1. Open Terminal and go to this folder: `cd tools/ble-setup`
2. Make a Python environment for the tool: `python3 -m venv .venv`
3. Install what it needs: `.venv/bin/pip install -r requirements.txt`
4. The first Bluetooth command makes macOS ask whether Terminal may use Bluetooth. Click **Allow**.
   If you missed it: System Settings, Privacy & Security, Bluetooth, turn Terminal on, then run the
   command again.

## Use

1. See which boards are waiting for setup: `.venv/bin/python ble_setup.py scan`
   Each line shows the board's name (`Lumen-xxxx`, the same as its hotspot) and its state.
2. Check a board: `.venv/bin/python ble_setup.py info Lumen-xxxx`
   It prints the firmware version and the board's device ID.
3. Make it pulse white to find it: `.venv/bin/python ble_setup.py identify Lumen-xxxx`
4. Send Wi-Fi: `.venv/bin/python ble_setup.py wifi Lumen-xxxx --ssid "Network name"`
   - It asks for the Wi-Fi password, then the board's hotspot passphrase (the one the claim
     returns). Typing is hidden. Neither is ever printed or saved.
   - Add `--device <device ID>` to refuse any board with a different device ID.
   - It ends with `online` and the board's address, or with the reason it failed (for example
     `wrong_password` or `network_not_found`).
5. Ask a board what happened to the last Wi-Fi it was sent: `.venv/bin/python ble_setup.py status Lumen-xxxx`

`--plain` sends the Wi-Fi settings unencrypted, as a generic Improv app would. A board with a
hotspot passphrase must refuse it (`not authorized`); that refusal is one of the bench checks.

## Tests (no board needed)

`.venv/bin/python -m unittest test_lumen_ble`

They check the encryption against a known-answer vector computed with a separate implementation
(Node's crypto), and that a wrong passphrase, another board, a replay or any changed byte fails.
