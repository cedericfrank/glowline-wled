// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
#ifdef GLOWLINE_IMPROV_BLE

#include "glowline_ble.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"

namespace GlowlineBle {
namespace {

// ---- Improv Wi-Fi BLE 2.4 (https://www.improv-wifi.com/ble/) ----
const char* const SVC_UUID    = "00467768-6228-2272-4663-277478268000";
const char* const STATE_UUID  = "00467768-6228-2272-4663-277478268001";
const char* const ERROR_UUID  = "00467768-6228-2272-4663-277478268002";
const char* const RPC_UUID    = "00467768-6228-2272-4663-277478268003";
const char* const RESULT_UUID = "00467768-6228-2272-4663-277478268004";
const char* const CAPS_UUID   = "00467768-6228-2272-4663-277478268005";
const uint16_t SERVICE_DATA_UUID = 0x4677;

enum : uint8_t { ST_AUTH_REQUIRED = 0x01, ST_AUTHORIZED = 0x02, ST_PROVISIONING = 0x03, ST_PROVISIONED = 0x04 };
enum : uint8_t { ERR_NONE = 0x00, ERR_INVALID_RPC = 0x01, ERR_UNKNOWN_RPC = 0x02, ERR_UNABLE_TO_CONNECT = 0x03,
                 ERR_NOT_AUTHORIZED = 0x04, ERR_UNKNOWN = 0xFF /* also: busy connecting */ };
enum : uint8_t { CMD_WIFI = 0x01, CMD_IDENTIFY = 0x02, CMD_DEVICE_INFO = 0x03,
                 // Lumen's own commands (contract "BLE provisioning (Improv)"); generic Improv clients get
                 // "unknown command" for them.
                 CMD_LUMEN_WIFI = 0x80, CMD_LUMEN_CHALLENGE = 0x81, CMD_LUMEN_STATUS = 0x82 };
const uint8_t CAPS = 0x01 /* identify */ | 0x02 /* device info */;

// ---- Lumen's encrypted Wi-Fi command ----
// Plain Improv sends the Wi-Fi password unencrypted; a board with a hotspot passphrase refuses that
// (ERR_NOT_AUTHORIZED) and takes the Wi-Fi settings only encrypted with a key derived from the
// passphrase, which itself never crosses the air:
//   Kd  = PBKDF2-HMAC-SHA256(passphrase, "lumen-ble-1:" + deviceId, KDF_ITERATIONS, 32)
//   key = HKDF-SHA256(ikm Kd, salt boardNonce | appNonce, info "lumen-ble-wifi-1", 32)
//   0x80 data = appNonce(16) | iv(12) | AES-256-GCM(key, iv, aad "lumen-ble-wifi-1", Improv 0x01 data) | tag(16)
// The board nonce is fresh per connection and after every 0x80, so a recorded packet never decrypts
// again. A valid tag is the authorization. Any change here breaks every app build: contract first.
const uint32_t KDF_ITERATIONS = 10000;
const char KDF_SALT_PREFIX[] = "lumen-ble-1:";
const char HKDF_INFO[] = "lumen-ble-wifi-1";
const char GCM_AAD[] = "lumen-ble-wifi-1";
const size_t NONCE_LEN = 16, IV_LEN = 12, TAG_LEN = 16, KEY_LEN = 32;
const int MAX_BAD_TAGS = 5;  // then BLE closes until the next reboot

const size_t RX_MAX = 3 + 255;           // command, length, up to 255 data bytes, checksum
const unsigned long RX_STALE_MS = 2000;  // a half-received packet older than this is dropped

enum class Phase : uint8_t { OFF, ON, RELEASED };
Phase phase = Phase::OFF;

Identity ident;
WifiHandler wifiHandler;
IdentifyHandler identifyHandler;

BLECharacteristic* chState = nullptr;
BLECharacteristic* chError = nullptr;
BLECharacteristic* chResult = nullptr;

uint8_t state = ST_AUTHORIZED;
uint8_t idleState = ST_AUTHORIZED;  // where a failed attempt returns to
uint8_t lastWifiCmd = CMD_WIFI;     // the command whose result carries the redirect URL
uint8_t kd[KEY_LEN];
bool kdReady = false;
uint8_t boardNonce[NONCE_LEN];
int badTags = 0;
const char* wifiState = "idle";  // CMD_LUMEN_STATUS: idle, connecting, connected, failed
String wifiDetail = "";          // the reason when failed, the URL when connected

// Written by the BLE host task (callbacks), read by loop().
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t rxBuf[RX_MAX];
size_t rxLen = 0;
unsigned long rxAt = 0;
uint8_t pending[RX_MAX];
size_t pendingLen = 0;
volatile bool pendingReady = false;
volatile bool rxOverflow = false;
volatile bool centralConnected = false;
volatile bool sawConnect = false;
volatile bool sawDisconnect = false;

void logHeap(const char* label) {
  Serial.printf("glowline ble: %s, internal heap free %u, largest block %u\n", label,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void newBoardNonce() { esp_fill_random(boardNonce, sizeof(boardNonce)); }

// A 128-bit UUID string as the 16 little-endian bytes an advertisement carries.
void uuidToAdvBytes(const char* s, uint8_t out[16]) {
  uint8_t be[16];
  int n = 0;
  for (const char* p = s; *p && n < 32; p++) {
    char c = *p;
    int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (v < 0) continue;  // dashes
    if (n % 2 == 0) be[n / 2] = (uint8_t)(v << 4); else be[n / 2] |= (uint8_t)v;
    n++;
  }
  for (int i = 0; i < 16; i++) out[i] = be[15 - i];
}

// Improv 2.1+: service UUID and service data (state, capabilities) together in the advertisement
// itself (3 + 18 + 10 = 31 bytes, the legacy maximum); the name goes in the scan response.
void updateAdvertising() {
  uint8_t adv[31];
  size_t n = 0;
  adv[n++] = 2;  adv[n++] = 0x01; adv[n++] = 0x06;   // flags: LE general discoverable, no BR/EDR
  adv[n++] = 17; adv[n++] = 0x07;                    // complete list of 128-bit service UUIDs
  uuidToAdvBytes(SVC_UUID, adv + n); n += 16;
  adv[n++] = 9;  adv[n++] = 0x16;                    // service data, 16-bit UUID
  adv[n++] = SERVICE_DATA_UUID & 0xFF; adv[n++] = SERVICE_DATA_UUID >> 8;
  adv[n++] = state; adv[n++] = CAPS; adv[n++] = 0; adv[n++] = 0; adv[n++] = 0; adv[n++] = 0;
  BLEAdvertisementData advData;
  advData.addData((char*)adv, n);

  uint8_t rsp[31];
  size_t nameLen = ident.advName.length() > 29 ? 29 : ident.advName.length();
  rsp[0] = (uint8_t)(nameLen + 1); rsp[1] = 0x09;    // complete local name
  memcpy(rsp + 2, ident.advName.c_str(), nameLen);
  BLEAdvertisementData rspData;
  rspData.addData((char*)rsp, nameLen + 2);

  BLEAdvertising* a = BLEDevice::getAdvertising();
  a->setAdvertisementData(advData);  // NimBLE: applied at once, also while advertising
  a->setScanResponseData(rspData);
}

void setState(uint8_t s) {
  state = s;
  chState->setValue(&state, 1);
  chState->notify();
  updateAdvertising();
}

void setError(uint8_t e) {
  chError->setValue(&e, 1);
  chError->notify();
}

// RPC result: command, data length, then each string as length + bytes, then the checksum.
void sendResult(uint8_t cmd, const String* strings, size_t count) {
  uint8_t out[RX_MAX];
  size_t n = 2;
  for (size_t i = 0; i < count; i++) {
    size_t len = strings[i].length();
    if (len > 255 || n + 1 + len + 1 > sizeof(out)) break;  // would not fit: the rest is left out
    out[n++] = (uint8_t)len;
    memcpy(out + n, strings[i].c_str(), len);
    n += len;
  }
  out[0] = cmd;
  out[1] = (uint8_t)(n - 2);
  uint8_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += out[i];
  out[n++] = sum;
  chResult->setValue(out, n);
  chResult->notify();
}

String hex(const uint8_t* b, size_t n) {
  static const char digits[] = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += digits[b[i] >> 4]; s += digits[b[i] & 0x0F]; }
  return s;
}

// Improv 0x01 data: SSID length, SSID, password length, password. Exactly that long; SSID 1-32
// bytes, password 0-64 (WLED's clientPass holds 64 + NUL).
bool takeWifiData(const uint8_t* d, size_t len, uint8_t cmd) {
  if (len < 2) return false;
  size_t ssidLen = d[0];
  if (ssidLen < 1 || ssidLen > 32 || len < 2 + ssidLen) return false;
  size_t passLen = d[1 + ssidLen];
  if (passLen > 64 || len != 2 + ssidLen + passLen) return false;
  char ssid[33], pass[65];
  memcpy(ssid, d + 1, ssidLen); ssid[ssidLen] = 0;
  memcpy(pass, d + 2 + ssidLen, passLen); pass[passLen] = 0;
  lastWifiCmd = cmd;
  wifiState = "connecting";
  wifiDetail = "";
  setError(ERR_NONE);
  setState(ST_PROVISIONING);
  Serial.println(F("glowline ble: Wi-Fi settings received, connecting"));  // never the SSID or password
  if (wifiHandler) wifiHandler(ssid, pass);
  mbedtls_platform_zeroize(pass, sizeof(pass));
  return true;
}

bool deriveKd() {
  if (ident.passphrase.length() == 0) return false;
  String salt = String(KDF_SALT_PREFIX) + ident.deviceId;
  unsigned long t0 = millis();
  int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                         (const unsigned char*)ident.passphrase.c_str(), ident.passphrase.length(),
                                         (const unsigned char*)salt.c_str(), salt.length(),
                                         KDF_ITERATIONS, KEY_LEN, kd);
  Serial.printf("glowline ble: key derivation %s in %lu ms\n", rc == 0 ? "done" : "FAILED", millis() - t0);
  return rc == 0;
}

// 0x80: decrypt and authenticate, then take the Wi-Fi data inside. Every attempt uses the board
// nonce up, so the next one needs a fresh CMD_LUMEN_CHALLENGE.
void lumenWifi(const uint8_t* d, size_t len) {
  if (!kdReady) { setError(ERR_NOT_AUTHORIZED); return; }  // no passphrase: plain 0x01 is the path
  if (len < NONCE_LEN + IV_LEN + 3 + TAG_LEN) { setError(ERR_INVALID_RPC); return; }
  const uint8_t* appNonce = d;
  const uint8_t* iv = d + NONCE_LEN;
  const uint8_t* ct = iv + IV_LEN;
  size_t ctLen = len - NONCE_LEN - IV_LEN - TAG_LEN;
  const uint8_t* tag = ct + ctLen;

  uint8_t salt[2 * NONCE_LEN], key[KEY_LEN], plain[255];
  memcpy(salt, boardNonce, NONCE_LEN);
  memcpy(salt + NONCE_LEN, appNonce, NONCE_LEN);
  newBoardNonce();
  int rc = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt), kd, KEY_LEN,
                        (const unsigned char*)HKDF_INFO, sizeof(HKDF_INFO) - 1, key, KEY_LEN);
  if (rc == 0) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_LEN * 8);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&gcm, ctLen, iv, IV_LEN, (const unsigned char*)GCM_AAD, sizeof(GCM_AAD) - 1,
                                               tag, TAG_LEN, ct, plain);
    mbedtls_gcm_free(&gcm);
  }
  mbedtls_platform_zeroize(key, sizeof(key));
  if (rc != 0) {
    badTags++;
    Serial.printf("glowline ble: encrypted Wi-Fi did not authenticate (%d of %d)\n", badTags, MAX_BAD_TAGS);
    setError(ERR_NOT_AUTHORIZED);
    if (badTags >= MAX_BAD_TAGS) release("too many attempts with the wrong key");
    return;
  }
  if (!takeWifiData(plain, ctLen, CMD_LUMEN_WIFI)) setError(ERR_INVALID_RPC);
  mbedtls_platform_zeroize(plain, sizeof(plain));
}

void handlePacket(const uint8_t* p, size_t len) {
  if (len < 3 || len != (size_t)p[1] + 3) { setError(ERR_INVALID_RPC); return; }
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < len; i++) sum += p[i];
  if (sum != p[len - 1]) { setError(ERR_INVALID_RPC); return; }
  uint8_t cmd = p[0];
  const uint8_t* d = p + 2;
  size_t dLen = p[1];
  bool busy = state == ST_PROVISIONING || state == ST_PROVISIONED;

  switch (cmd) {
    case CMD_DEVICE_INFO: {
      String info[4] = { "Lumen", ident.fwVersion, "ESP32-S3", ident.deviceId };
      sendResult(CMD_DEVICE_INFO, info, 4);
      break;
    }
    case CMD_IDENTIFY:
      if (identifyHandler) identifyHandler();
      break;
    case CMD_WIFI:
      // A board with a passphrase takes Wi-Fi settings only encrypted (CMD_LUMEN_WIFI).
      if (state != ST_AUTHORIZED) { setError(busy ? ERR_UNKNOWN : ERR_NOT_AUTHORIZED); break; }
      if (!takeWifiData(d, dLen, CMD_WIFI)) setError(ERR_INVALID_RPC);
      break;
    case CMD_LUMEN_CHALLENGE: {
      String s[1] = { hex(boardNonce, NONCE_LEN) };
      sendResult(CMD_LUMEN_CHALLENGE, s, 1);
      break;
    }
    case CMD_LUMEN_WIFI:
      if (busy) { setError(ERR_UNKNOWN); break; }
      lumenWifi(d, dLen);
      break;
    case CMD_LUMEN_STATUS: {
      String s[2] = { wifiState, wifiDetail };
      sendResult(CMD_LUMEN_STATUS, s, 2);
      break;
    }
    default:
      setError(ERR_UNKNOWN_RPC);
      break;
  }
}

// ---- BLE host task: copy and return (BLECharacteristic.cpp: onWrite runs before the write response) ----
class RpcCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    size_t n = c->getLength();
    const uint8_t* d = c->getData();
    unsigned long now = millis();
    portENTER_CRITICAL(&mux);
    if (rxLen && now - rxAt > RX_STALE_MS) rxLen = 0;
    if (pendingReady || rxLen + n > RX_MAX) {
      rxLen = 0;
      rxOverflow = true;
    } else {
      memcpy(rxBuf + rxLen, d, n);
      rxLen += n;
      rxAt = now;
      // Improv packets longer than one write arrive in pieces; the second byte says how long it is.
      if (rxLen >= 2 && rxLen >= (size_t)rxBuf[1] + 3) {
        memcpy(pending, rxBuf, rxLen);
        pendingLen = rxLen;
        pendingReady = true;
        rxLen = 0;
      }
    }
    portEXIT_CRITICAL(&mux);
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { centralConnected = true; sawConnect = true; }
  void onDisconnect(BLEServer*) override { centralConnected = false; sawDisconnect = true; }
};

}  // namespace

bool start(const Identity& id, WifiHandler onWifi, IdentifyHandler onIdentify) {
  if (phase != Phase::OFF) return false;
  ident = id;
  wifiHandler = onWifi;
  identifyHandler = onIdentify;
  logHeap("starting");
  kdReady = deriveKd();
  if (ident.passphrase.length() > 0 && !kdReady) {
    // Without the key a board with a passphrase can't take Wi-Fi at all; don't advertise a dead end.
    Serial.println(F("glowline ble: not started, the key could not be derived"));
    return false;
  }
  idleState = kdReady ? ST_AUTH_REQUIRED : ST_AUTHORIZED;
  state = idleState;
  newBoardNonce();

  if (!BLEDevice::init(ident.advName)) {
    Serial.println(F("glowline ble: not started, BLEDevice::init() failed"));
    return false;
  }
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService* svc = server->createService(SVC_UUID);
  chState = svc->createCharacteristic(STATE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chError = svc->createCharacteristic(ERROR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* chRpc = svc->createCharacteristic(RPC_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  chResult = svc->createCharacteristic(RESULT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* chCaps = svc->createCharacteristic(CAPS_UUID, BLECharacteristic::PROPERTY_READ);
  chRpc->setCallbacks(new RpcCallbacks());
  uint8_t none = ERR_NONE, caps = CAPS;
  chState->setValue(&state, 1);
  chError->setValue(&none, 1);
  chCaps->setValue(&caps, 1);
  svc->start();
  updateAdvertising();
  BLEDevice::startAdvertising();
  phase = Phase::ON;
  Serial.printf("glowline ble: advertising as %s (%s)\n", ident.advName.c_str(),
                kdReady ? "encrypted Wi-Fi only" : "no passphrase, plain Improv Wi-Fi accepted");
  logHeap("up");
  return true;
}

void loop() {
  if (phase != Phase::ON) return;
  if (sawConnect) {
    sawConnect = false;
    Serial.println(F("glowline ble: phone connected"));
  }
  if (sawDisconnect) {
    sawDisconnect = false;
    newBoardNonce();  // a new connection gets a new challenge
    Serial.println(F("glowline ble: phone disconnected"));
  }
  if (rxOverflow) {
    rxOverflow = false;
    setError(ERR_INVALID_RPC);
  }
  if (!pendingReady) return;
  uint8_t pkt[RX_MAX];
  size_t len;
  portENTER_CRITICAL(&mux);
  len = pendingLen;
  memcpy(pkt, pending, len);
  pendingReady = false;
  portEXIT_CRITICAL(&mux);
  handlePacket(pkt, len);
  mbedtls_platform_zeroize(pkt, sizeof(pkt));
}

bool active() { return phase == Phase::ON; }
bool released() { return phase == Phase::RELEASED; }

void reportConnected(const char* url) {
  if (phase != Phase::ON) return;
  wifiState = "connected";
  wifiDetail = url;
  setState(ST_PROVISIONED);
  String s[1] = { url };
  sendResult(lastWifiCmd, s, 1);
  Serial.println(F("glowline ble: Wi-Fi connected, told the phone"));
}

void reportFailed(const char* reason) {
  if (phase != Phase::ON) return;
  wifiState = "failed";
  wifiDetail = reason;
  setError(ERR_UNABLE_TO_CONNECT);
  setState(idleState);
  Serial.printf("glowline ble: Wi-Fi did not connect (%s), told the phone\n", reason);
}

void release(const char* why) {
  if (phase != Phase::ON) return;
  phase = Phase::RELEASED;
  Serial.printf("glowline ble: releasing (%s)\n", why);
  BLEDevice::deinit(true);  // frees the controller's memory; BLE can't start again until a reboot
  chState = chError = chResult = nullptr;
  mbedtls_platform_zeroize(kd, sizeof(kd));
  kdReady = false;
  if (ident.passphrase.length()) mbedtls_platform_zeroize((void*)ident.passphrase.c_str(), ident.passphrase.length());
  ident.passphrase = "";
  logHeap("released");
}

const char* stateName() {
  switch (phase) {
    case Phase::ON: return "on";
    case Phase::RELEASED: return "released";
    default: return "off";
  }
}

}  // namespace GlowlineBle

#endif  // GLOWLINE_IMPROV_BLE
