#!/usr/bin/env python3
"""REQ-165 bench: read the ISR-gap probe (lumen.probe in /json/info) and name the interrupted code.

The probe build (esp32s3_lumen_dev_BENCH_ISRGAP_DO_NOT_SHIP, usermods/glowline/glowline_isrgap.h)
records interrupt hold-offs with the interrupted address. This fetches /json/info from the board (or
reads a saved copy), resolves each address with addr2line on the build's ELF and prints a table.

Python 3 standard library only. Usage:
  python3 tools/bench/req165_probe_resolve.py [--host lumen-b4170c.local | --file info.json]
                                               [--elf .pio/build/esp32s3_lumen_dev_BENCH_ISRGAP_DO_NOT_SHIP/firmware.elf]
"""
import argparse
import json
import os
import subprocess
import urllib.request

ADDR2LINE = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line")


def resolve(elf, pcs):
    pcs = [p for p in pcs if p and p != "0x00000000"]
    if not pcs or not os.path.exists(elf):
        return {}
    out = subprocess.run([ADDR2LINE, "-e", elf, "-f", "-C", "-s", *pcs], capture_output=True, text=True).stdout
    lines = out.splitlines()
    return {pc: f"{lines[2 * i]} ({lines[2 * i + 1]})" for i, pc in enumerate(pcs) if 2 * i + 1 < len(lines)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="lumen-b4170c.local")
    ap.add_argument("--file", help="a saved /json/info instead of fetching it")
    ap.add_argument("--elf", default=".pio/build/esp32s3_lumen_dev_BENCH_ISRGAP_DO_NOT_SHIP/firmware.elf")
    a = ap.parse_args()
    if a.file:
        with open(a.file) as f:
            info = json.load(f)
    else:
        with urllib.request.urlopen(f"http://{a.host}/json/info", timeout=5) as r:
            info = json.load(r)
    lumen = info.get("lumen", {})
    probe = lumen.get("probe")
    if not probe:
        raise SystemExit(f"No lumen.probe in /json/info (firmware {lumen.get('fw')}): not the probe build.")
    print(f"firmware {lumen.get('fw')}, up {probe.get('up_s')} s, length {info['leds']['count']}, "
          f"frame window {probe.get('frame_us')} us, ticks {probe.get('ticks')}")
    print(f"hold-offs >=100/250/1000 us: {probe['n100']}/{probe['n250']}/{probe['n1000']}, max {probe['max_us']} us; "
          f"during a frame: {probe['in100']}/{probe['in250']}/{probe['in1000']}, max {probe['in_max_us']} us")
    print(f"heap walk (largest free block): last {probe['walk_us']} us, max {probe['walk_max_us']} us; "
          f"largest {probe['largest']} B; blocks {probe['blocks']} ({probe['free_blocks']} free)")
    gaps = probe.get("last", []) + probe.get("top", [])
    names = resolve(a.elf, [g["pc"] for g in gaps] + [g.get("epc1", "") for g in gaps]
                    + [w for g in gaps for w in g.get("w", [])] + [b for g in gaps for b in g.get("bt", [])])
    for title in ("last", "top"):
        print(f"\n{title}:")
        for g in sorted(probe.get(title, []), key=lambda g: g["ms"]):
            show = g.get("show")
            where = f"{show} us after frame start" if show is not None and show < probe.get("frame_us", 0) else "no frame"
            print(f"  {g['us']:>6} us at {g['ms']:>8} ms  {g['task']:<10} other={g.get('other', ''):<10} {g['ph']:<12} {where}")
            for label, addr in (("pc", g["pc"]), ("epc1", g.get("epc1")), ("a0", (g.get("w") or [None] * 4)[3])):
                if addr:
                    print(f"{'':>12}{label:<5} {addr}  {names.get(addr, '')}")
            for b in g.get("bt", []):
                print(f"{'':>12}  <- {b}  {names.get(b, '')}")


if __name__ == "__main__":
    main()
