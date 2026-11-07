#!/usr/bin/env python3
"""REQ-165 bench: send views to a Lumen board over WLED's /ws from this Mac, the way the app does.

Splits round A (a view tap in the app lights LEDs past the length on 1.19.3, editor previews don't).
A view tap sends the look twice: the app over Wi-Fi (/ws), then the server a few hundred ms later
with a preset save (`psave`). This script sends:
  round 1: the app's Wi-Fi message only
  round 2: the same, then a second copy 300 ms later with `psave` to slot 250 (deleted at the end)
Looks are copied from the board after you tap your own views in the app (step 1).

Python 3 standard library only. Nothing secret is read or sent. Usage:
  python3 tools/bench/req165_ws_replay.py [--host lumen-b4170c.local] [--looks 4] [--steps 8]
"""
import argparse
import base64
import json
import os
import socket
import struct
import time
import urllib.request

SEG_KEYS = ("fx", "pal", "col", "sx", "ix", "bri", "mi", "rev")  # contracts/api.md "Added by the server"
MAX_SEG_ID = 15
BENCH_SLOT = 250


def get_json(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=5) as r:
        return json.load(r)


class Ws:
    """Minimal WebSocket client: text frames out, replies read and dropped."""

    def __init__(self, host):
        self.sock = socket.create_connection((host, 80), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SystemExit("The board closed the connection before /ws opened.")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise SystemExit("The board did not open /ws: " + head.split(b"\r\n", 1)[0].decode(errors="replace"))
        self.sock.setblocking(False)

    def send(self, obj):
        data = json.dumps(obj, separators=(",", ":")).encode()
        if len(data) > 1400:
            raise SystemExit(f"Message is {len(data)} bytes; WLED takes one frame of at most ~1428.")
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.sock.setblocking(True)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        self.sock.setblocking(False)
        self.drain()

    def drain(self):
        try:
            while self.sock.recv(65536):
                pass
        except (BlockingIOError, socket.timeout):
            pass

    def close(self):
        self.sock.close()


def app_patch(state, length):
    """The app's view tap: on, bri, the view's segments, every other id up to 15 deleted."""
    segs = []
    src = [s for s in state.get("seg", []) if s.get("stop", 0) > s.get("start", 0)]
    for i, s in enumerate(src):
        seg = {k: s[k] for k in SEG_KEYS if k in s}
        seg["id"] = i
        if len(src) == 1:
            seg["start"], seg["stop"] = 0, length
        else:
            seg["start"], seg["stop"] = s["start"], s["stop"]
        segs.append(seg)
    segs += [{"id": i, "stop": 0} for i in range(len(segs), MAX_SEG_ID + 1)]
    return {"on": True, "bri": state.get("bri", 128), "seg": segs}


def ask(prompt):
    return input(f"\n{prompt}\n> ").strip()


def run_round(ws, looks, steps, with_save):
    for n in range(steps):
        patch = looks[n % len(looks)]
        ws.send(patch)
        if with_save:
            time.sleep(0.3)
            ws.send(dict(patch, psave=BENCH_SLOT, sb=True, n="REQ-165 bench"))
            time.sleep(1.7)
        else:
            time.sleep(2.0)
        print(f"  look {n + 1}/{steps} sent")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="lumen-b4170c.local")
    ap.add_argument("--looks", type=int, default=4, help="views to copy from the board (default 4)")
    ap.add_argument("--steps", type=int, default=8, help="looks sent per round (default 8)")
    a = ap.parse_args()

    info = get_json(a.host, "/json/info")
    length = info["leds"]["count"]
    print(f"Board {info.get('name')} at {a.host}: firmware {info.get('lumen', {}).get('fw')}, length {length}.")

    print("\nStep 1: copy your views from the board.")
    looks = []
    for i in range(a.looks):
        ask(f"In Lumen, tap a bright view ({'Party first' if i == 0 else 'a different one'}), "
            f"wait 3 s, then press Enter. ({i + 1} of {a.looks})")
        looks.append(app_patch(get_json(a.host, "/json/state"), length))
        print(f"  copied: effect {looks[-1]['seg'][0].get('fx')}, brightness {looks[-1]['bri']}")

    ask("Step 2: close Lumen fully (swipe it away). Then unplug the strip's power, wait 5 s and plug it "
        "back in. Wait until the strip shows its look again, check LEDs 321 and up are dark, then press Enter.")
    print(f"  WLED /ws clients now: {get_json(a.host, '/json/info').get('ws')} (the app closed: expect 1 or 0)")

    ws = Ws(a.host)
    try:
        print("\nRound 1: the app's Wi-Fi message only, a new look every 2 s.")
        run_round(ws, looks, a.steps, with_save=False)
        r1 = ask("Round 1: were LEDs past the length lit or dark? Type lit or dark.")

        ask("Unplug the strip's power, wait 5 s, plug it back in, wait for its look, check the LEDs past "
            "the length are dark, then press Enter.")
        ws.close()
        ws = Ws(a.host)
        print("\nRound 2: the app's message, then a second copy 300 ms later with a preset save.")
        run_round(ws, looks, a.steps, with_save=True)
        r2 = ask("Round 2: were LEDs past the length lit or dark? Type lit or dark.")

        ws.send({"pdel": BENCH_SLOT})
        print(f"  bench preset {BENCH_SLOT} deleted.")
    finally:
        ws.close()

    print(f"\nResult to send back: round 1 (Wi-Fi only) {r1}; round 2 (Wi-Fi + preset save copy) {r2}; "
          f"firmware {info.get('lumen', {}).get('fw')}, length {length}.")


if __name__ == "__main__":
    main()
