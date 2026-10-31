# SPDX-License-Identifier: EUPL-1.2
# Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
"""Offline tests: python3 -m unittest test_lumen_ble (from this folder, in its venv)."""
import unittest

from cryptography.exceptions import InvalidTag

import lumen_ble as lb

# Known-answer vector, computed with Node's built-in crypto (pbkdf2Sync, hkdfSync, aes-256-gcm),
# which shares no code with lumen_ble.py. The same vector is in lumen-shared contracts/firmware.md
# "BLE provisioning (Improv)" for the app.
V = {
    "passphrase": "ABCDEFGHJKLM",
    "deviceId": "00000000-0000-4000-8000-000000000001",
    "boardNonce": "000102030405060708090a0b0c0d0e0f",
    "appNonce": "101112131415161718191a1b1c1d1e1f",
    "iv": "202122232425262728292a2b",
    "ssid": "LumenTest",
    "password": "correct horse 42",
    "kd": "8ed15ee1d0773c265f94a92b276581428dcc7c6d8e3b5196f6cfaecdfd183786",
    "key": "96a669c7d69a725536a090515756cdd45fbeb19603b4dd4b5bf3cca1b0900a10",
    "packet": "8047101112131415161718191a1b1c1d1e1f202122232425262728292a2b9c5d53b28b243114d414572f5f02a9e5278de7510924e511ca8b2f75457f5c120c709ac0fde1bb1fa4775588",
}
BOARD = bytes.fromhex(V["boardNonce"])
APP = bytes.fromhex(V["appNonce"])
IV = bytes.fromhex(V["iv"])


class KnownAnswer(unittest.TestCase):
    def test_kd(self):
        self.assertEqual(lb.derive_kd(V["passphrase"], V["deviceId"]).hex(), V["kd"])

    def test_session_key(self):
        self.assertEqual(lb.session_key(bytes.fromhex(V["kd"]), BOARD, APP).hex(), V["key"])

    def test_packet(self):
        data = lb.lumen_wifi_data(bytes.fromhex(V["kd"]), BOARD, V["ssid"], V["password"], app_nonce=APP, iv=IV)
        self.assertEqual(lb.rpc_packet(lb.CMD_LUMEN_WIFI, data).hex(), V["packet"])

    def test_hkdf_rfc5869_case1(self):
        # RFC 5869 A.1, so hkdf_sha256() is right on its own, not only against Node.
        okm = lb.hkdf_sha256(bytes.fromhex("0b" * 22), bytes.fromhex("000102030405060708090a0b0c"),
                             bytes.fromhex("f0f1f2f3f4f5f6f7f8f9"), 42)
        self.assertEqual(okm.hex(), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                                    "34007208d5b887185865")


class Encryption(unittest.TestCase):
    kd = bytes.fromhex(V["kd"])

    def test_round_trip(self):
        data = lb.lumen_wifi_data(self.kd, BOARD, "Home", "hunter2hunter2")
        self.assertEqual(lb.open_lumen_wifi_data(self.kd, BOARD, data), lb.wifi_data("Home", "hunter2hunter2"))

    def test_wrong_passphrase_fails(self):
        data = lb.lumen_wifi_data(self.kd, BOARD, "Home", "hunter2hunter2")
        with self.assertRaises(InvalidTag):
            lb.open_lumen_wifi_data(lb.derive_kd("ABCDEFGHJKLN", V["deviceId"]), BOARD, data)

    def test_other_device_id_fails(self):
        data = lb.lumen_wifi_data(self.kd, BOARD, "Home", "hunter2hunter2")
        with self.assertRaises(InvalidTag):
            lb.open_lumen_wifi_data(lb.derive_kd(V["passphrase"], "another-device"), BOARD, data)

    def test_replay_with_a_new_board_nonce_fails(self):
        data = lb.lumen_wifi_data(self.kd, BOARD, "Home", "hunter2hunter2")
        with self.assertRaises(InvalidTag):
            lb.open_lumen_wifi_data(self.kd, bytes(16), data)

    def test_any_flipped_byte_fails(self):
        data = bytearray(lb.lumen_wifi_data(self.kd, BOARD, "Home", "pw"))
        for i in range(len(data)):
            tampered = bytearray(data)
            tampered[i] ^= 0x01
            with self.assertRaises(InvalidTag, msg=f"byte {i}"):
                lb.open_lumen_wifi_data(self.kd, BOARD, bytes(tampered))

    def test_fresh_nonces_each_time(self):
        a = lb.lumen_wifi_data(self.kd, BOARD, "Home", "pw")
        b = lb.lumen_wifi_data(self.kd, BOARD, "Home", "pw")
        self.assertNotEqual(a[:lb.NONCE_LEN + lb.IV_LEN], b[:lb.NONCE_LEN + lb.IV_LEN])

    def test_longest_settings_fit_one_packet(self):
        data = lb.lumen_wifi_data(self.kd, BOARD, "s" * 32, "p" * 64)
        self.assertEqual(len(data), 16 + 12 + (2 + 32 + 64) + 16)
        self.assertLessEqual(len(lb.rpc_packet(lb.CMD_LUMEN_WIFI, data)), 3 + 255)


class Packets(unittest.TestCase):
    def test_spec_example(self):
        # improv-wifi.com/ble: 01 1E 0C "MyWirelessAP" 10 "mysecurepassword" CS
        pkt = lb.rpc_packet(lb.CMD_WIFI, lb.wifi_data("MyWirelessAP", "mysecurepassword"))
        self.assertEqual(pkt[:3].hex(), "011e0c")
        self.assertEqual(pkt[-1], sum(pkt[:-1]) & 0xFF)

    def test_result_round_trip(self):
        strings =["Lumen", "1.15.0", "ESP32-S3", V["deviceId"]]
        data = b"".join(bytes([len(s)]) + s.encode() for s in strings)
        body = bytes([0x03, len(data)]) + data
        self.assertEqual(lb.parse_result(body + bytes([sum(body) & 0xFF])), (0x03, strings))

    def test_result_bad_checksum(self):
        body = bytes([0x81, 1, 0])
        with self.assertRaises(ValueError):
            lb.parse_result(body + bytes([(sum(body) + 1) & 0xFF]))

    def test_wifi_data_limits(self):
        with self.assertRaises(ValueError):
            lb.wifi_data("", "pw")
        with self.assertRaises(ValueError):
            lb.wifi_data("s" * 33, "pw")
        with self.assertRaises(ValueError):
            lb.wifi_data("s", "p" * 65)
        self.assertEqual(lb.wifi_data("s", ""), b"\x01s\x00")  # open network


if __name__ == "__main__":
    unittest.main()
