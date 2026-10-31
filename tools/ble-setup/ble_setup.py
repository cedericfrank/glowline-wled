#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
"""Bench tool: set up a Lumen board's Wi-Fi over Bluetooth, as the app will (REQ-131/REQ-132).

  python ble_setup.py scan
  python ble_setup.py info Lumen-xxxx
  python ble_setup.py identify Lumen-xxxx
  python ble_setup.py wifi Lumen-xxxx --ssid "Home"            # encrypted; asks for both secrets
  python ble_setup.py wifi Lumen-xxxx --ssid "Home" --plain    # plain Improv (refused with a passphrase)
  python ble_setup.py status Lumen-xxxx

The Wi-Fi password and the hotspot passphrase are read at a hidden prompt, never from the command
line, and never printed. See readme.md.
"""
import argparse
import asyncio
import getpass
import sys

from bleak import BleakClient, BleakScanner

import lumen_ble as lb


def state_from_adv(adv) -> str:
    data = adv.service_data.get(lb.SERVICE_DATA_UUID)
    if not data:
        return "?"
    return lb.STATES.get(data[0], f"0x{data[0]:02x}")


async def find(name: str, timeout: float):
    found = await BleakScanner.find_device_by_filter(
        lambda d, adv: (adv.local_name or d.name) == name and lb.SVC_UUID in adv.service_uuids, timeout=timeout)
    if not found:
        sys.exit(f"{name} not found within {timeout:.0f} s (is the board on its setup hotspot? Bluetooth only runs then)")
    return found


class Board:
    """One connection: RPC writes, results and state/error notifications."""

    def __init__(self, client: BleakClient):
        self.client = client
        self.results = asyncio.Queue()
        self.events = asyncio.Queue()

    async def start(self):
        await self.client.start_notify(lb.RESULT_UUID, lambda _, d: self.results.put_nowait(bytes(d)))
        await self.client.start_notify(lb.STATE_UUID, lambda _, d: self.events.put_nowait(("state", d[0])))
        await self.client.start_notify(lb.ERROR_UUID, lambda _, d: self.events.put_nowait(("error", d[0])))

    async def read_byte(self, uuid: str) -> int:
        return (await self.client.read_gatt_char(uuid))[0]

    async def rpc(self, cmd: int, data: bytes = b"", want_result: bool = True, timeout: float = 5.0):
        await self.client.write_gatt_char(lb.RPC_UUID, lb.rpc_packet(cmd, data), response=True)
        if not want_result:
            return None
        try:
            got_cmd, strings = lb.parse_result(await asyncio.wait_for(self.results.get(), timeout))
        except asyncio.TimeoutError:
            err = await self.read_byte(lb.ERROR_UUID)
            sys.exit(f"no answer to command 0x{cmd:02x}; board error: {lb.ERRORS.get(err, hex(err))}")
        if got_cmd != cmd:
            sys.exit(f"answer for command 0x{got_cmd:02x}, expected 0x{cmd:02x}")
        return strings


async def cmd_scan(args):
    seen = await BleakScanner.discover(timeout=args.timeout, return_adv=True, service_uuids=[lb.SVC_UUID])
    rows = [(adv.local_name or d.name or "?", d.address, adv.rssi, state_from_adv(adv)) for d, adv in seen.values()]
    if not rows:
        print("no board advertising the Improv service")
    for name, addr, rssi, state in sorted(rows):
        print(f"{name:12}  {state:24}  rssi {rssi:4}  {addr}")


async def with_board(args, fn):
    device = await find(args.name, args.timeout)
    async with BleakClient(device) as client:
        board = Board(client)
        await board.start()
        await fn(board)


async def cmd_info(args):
    async def run(board):
        fw_name, version, chip, device_id = (await board.rpc(lb.CMD_DEVICE_INFO))[:4]
        state = await board.read_byte(lb.STATE_UUID)
        caps = await board.read_byte(lb.CAPS_UUID)
        print(f"firmware   {fw_name} {version} ({chip})")
        print(f"device ID  {device_id or '(none: blank board)'}")
        print(f"state      {lb.STATES.get(state, hex(state))}")
        print(f"caps       0x{caps:02x}")
    await with_board(args, run)


async def cmd_identify(args):
    async def run(board):
        await board.rpc(lb.CMD_IDENTIFY, want_result=False)
        print("sent; the strip should pulse white 3 times")
    await with_board(args, run)


async def cmd_status(args):
    async def run(board):
        state, detail = (await board.rpc(lb.CMD_LUMEN_STATUS) + ["", ""])[:2]
        print(f"wifi {state}{': ' + detail if detail else ''}")
    await with_board(args, run)


async def cmd_wifi(args):
    password = getpass.getpass(f"Wi-Fi password for {args.ssid} (empty for an open network): ")
    passphrase = "" if args.plain else getpass.getpass("Board's hotspot passphrase (from the claim): ").strip()

    async def run(board):
        device_id = (await board.rpc(lb.CMD_DEVICE_INFO))[3]
        if args.device and device_id != args.device:
            sys.exit(f"this board's device ID is {device_id!r}, not {args.device!r}: not sending anything")
        if args.plain:
            await board.rpc(lb.CMD_WIFI, lb.wifi_data(args.ssid, password), want_result=False)
        else:
            board_nonce = bytes.fromhex((await board.rpc(lb.CMD_LUMEN_CHALLENGE))[0])
            kd = lb.derive_kd(passphrase, device_id)
            data = lb.lumen_wifi_data(kd, board_nonce, args.ssid, password)
            await board.rpc(lb.CMD_LUMEN_WIFI, data, want_result=False)
        print("sent, waiting for the board (up to 40 s)")
        deadline = asyncio.get_running_loop().time() + 40
        while True:
            left = deadline - asyncio.get_running_loop().time()
            if left <= 0:
                sys.exit("no result within 40 s")
            try:
                kind, value = await asyncio.wait_for(board.events.get(), left)
            except asyncio.TimeoutError:
                continue
            if kind == "state":
                print(f"state: {lb.STATES.get(value, hex(value))}")
                if value == 0x04:
                    url = lb.parse_result(await asyncio.wait_for(board.results.get(), 5))[1]
                    print(f"online; redirect URL {url[0] if url else '(none)'}")
                    return
            elif kind == "error" and value != 0x00:
                print(f"error: {lb.ERRORS.get(value, hex(value))}")
                if value == 0x03:
                    state, detail = (await board.rpc(lb.CMD_LUMEN_STATUS) + ["", ""])[:2]
                    print(f"reason: {detail or state}")
                sys.exit(1)
    await with_board(args, run)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--timeout", type=float, default=10.0, help="scan time in seconds")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    for c in ("info", "identify", "status"):
        sub.add_parser(c).add_argument("name", help="Lumen-xxxx")
    w = sub.add_parser("wifi")
    w.add_argument("name", help="Lumen-xxxx")
    w.add_argument("--ssid", required=True)
    w.add_argument("--device", help="refuse unless the board reports this device ID")
    w.add_argument("--plain", action="store_true", help="plain Improv 0x01 (no encryption)")
    args = p.parse_args()
    fn = {"scan": cmd_scan, "info": cmd_info, "identify": cmd_identify, "status": cmd_status, "wifi": cmd_wifi}[args.cmd]
    asyncio.run(fn(args))


if __name__ == "__main__":
    main()
