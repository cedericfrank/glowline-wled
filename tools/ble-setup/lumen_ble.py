# SPDX-License-Identifier: EUPL-1.2
# Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
"""Improv Wi-Fi BLE packets and Lumen's encrypted Wi-Fi command (no Bluetooth here).

Mirrors usermods/glowline/glowline_ble.cpp; the wire format is lumen-shared contracts/firmware.md
"BLE provisioning (Improv)". Pure functions, so test_lumen_ble.py can check them offline.
"""
import hashlib
import hmac
import os

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

SVC_UUID = "00467768-6228-2272-4663-277478268000"
STATE_UUID = "00467768-6228-2272-4663-277478268001"
ERROR_UUID = "00467768-6228-2272-4663-277478268002"
RPC_UUID = "00467768-6228-2272-4663-277478268003"
RESULT_UUID = "00467768-6228-2272-4663-277478268004"
CAPS_UUID = "00467768-6228-2272-4663-277478268005"
SERVICE_DATA_UUID = "00004677-0000-1000-8000-00805f9b34fb"

CMD_WIFI = 0x01
CMD_IDENTIFY = 0x02
CMD_DEVICE_INFO = 0x03
CMD_LUMEN_WIFI = 0x80
CMD_LUMEN_CHALLENGE = 0x81
CMD_LUMEN_STATUS = 0x82

STATES = {0x01: "authorization required", 0x02: "authorized", 0x03: "provisioning", 0x04: "provisioned"}
ERRORS = {0x00: "none", 0x01: "invalid RPC packet", 0x02: "unknown RPC command", 0x03: "unable to connect",
          0x04: "not authorized", 0xFF: "unknown error (or busy)"}

KDF_ITERATIONS = 10000
KDF_SALT_PREFIX = b"lumen-ble-1:"
HKDF_INFO = b"lumen-ble-wifi-1"
GCM_AAD = b"lumen-ble-wifi-1"
NONCE_LEN = 16
IV_LEN = 12
KEY_LEN = 32


def checksum(data: bytes) -> int:
    return sum(data) & 0xFF


def rpc_packet(cmd: int, data: bytes = b"") -> bytes:
    if len(data) > 255:
        raise ValueError("RPC data is longer than 255 bytes")
    body = bytes([cmd, len(data)]) + data
    return body + bytes([checksum(body)])


def parse_result(packet: bytes):
    """An RPC result: (command, [strings]). Raises ValueError on a bad length or checksum."""
    if len(packet) < 3 or len(packet) != packet[1] + 3:
        raise ValueError("RPC result has the wrong length")
    if checksum(packet[:-1]) != packet[-1]:
        raise ValueError("RPC result checksum mismatch")
    data, strings, i = packet[2:-1], [], 0
    while i < len(data):
        n = data[i]
        if i + 1 + n > len(data):
            raise ValueError("RPC result string runs past the end")
        strings.append(data[i + 1:i + 1 + n].decode("utf-8"))
        i += 1 + n
    return packet[0], strings


def wifi_data(ssid: str, password: str) -> bytes:
    """Improv 0x01 data: SSID length, SSID, password length, password."""
    s, p = ssid.encode("utf-8"), password.encode("utf-8")
    if not 1 <= len(s) <= 32:
        raise ValueError("SSID must be 1-32 bytes")
    if len(p) > 64:
        raise ValueError("password must be at most 64 bytes")
    return bytes([len(s)]) + s + bytes([len(p)]) + p


def derive_kd(passphrase: str, device_id: str) -> bytes:
    return hashlib.pbkdf2_hmac("sha256", passphrase.encode("utf-8"), KDF_SALT_PREFIX + device_id.encode("utf-8"),
                               KDF_ITERATIONS, KEY_LEN)


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 with SHA-256 (what mbedtls_hkdf() computes)."""
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    okm, block, counter = b"", b"", 1
    while len(okm) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        okm += block
        counter += 1
    return okm[:length]


def session_key(kd: bytes, board_nonce: bytes, app_nonce: bytes) -> bytes:
    return hkdf_sha256(kd, board_nonce + app_nonce, HKDF_INFO, KEY_LEN)


def lumen_wifi_data(kd: bytes, board_nonce: bytes, ssid: str, password: str,
                    app_nonce: bytes = None, iv: bytes = None) -> bytes:
    """0x80 data: app nonce | iv | AES-256-GCM ciphertext of the Improv 0x01 data | tag."""
    if len(board_nonce) != NONCE_LEN:
        raise ValueError("board nonce must be 16 bytes")
    app_nonce = app_nonce if app_nonce is not None else os.urandom(NONCE_LEN)
    iv = iv if iv is not None else os.urandom(IV_LEN)
    key = session_key(kd, board_nonce, app_nonce)
    sealed = AESGCM(key).encrypt(iv, wifi_data(ssid, password), GCM_AAD)  # ciphertext | 16-byte tag
    return app_nonce + iv + sealed


def open_lumen_wifi_data(kd: bytes, board_nonce: bytes, data: bytes) -> bytes:
    """The board's side, for tests: returns the Improv 0x01 data or raises InvalidTag."""
    app_nonce, iv, sealed = data[:NONCE_LEN], data[NONCE_LEN:NONCE_LEN + IV_LEN], data[NONCE_LEN + IV_LEN:]
    return AESGCM(session_key(kd, board_nonce, app_nonce)).decrypt(iv, sealed, GCM_AAD)
