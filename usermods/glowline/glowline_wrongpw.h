#pragma once
// Back to setup mode after a rejected password (REQ-135, lumen-shared contracts/firmware.md "BLE
// provisioning (Improv)"). A boot whose Wi-Fi connected first has no Bluetooth (its memory is given
// back for TLS), so when the saved network later keeps rejecting the password (the owner changed it),
// the board restarts once: the next boot's saved Wi-Fi fails, and the hotspot and Bluetooth come up.
//
// Fires when the saved network has rejected the password at least FAILS times, the first and the
// last at least SPAN_MS apart, with no association in between. Other failures ("network not found",
// weak signal: a router that is off or rebooting) neither count nor reset the count, since scans
// miss a network now and then between rejected passwords (bench, REQ-131). An association resets it.
//
// What counts as a rejection is rejects() below: the wrong-password codes, plus AUTH_EXPIRE, which an
// iPhone hotspot sends for a wrong password instead (bench 2026-11-05, REQ-135: ~100 times in a row, never
// with the right one). AUTH_EXPIRE can also come with the right password (Espressif took it out of their
// provisioning manager's "bad credentials" group, esp-idf b7db75e), so it counts here, where it takes
// FAILS over SPAN_MS with no join in between, but the app is still told "connection_failed" for it.
//
// Pure logic only, tested on the host: usermods/glowline/test/wrongpw_test.cpp (npm test).

#include <stdint.h>

namespace GlowlineWrongPw {

static const uint8_t FAILS = 3;

// ESP-IDF wifi_err_reason_t values (esp_wifi_types_generic.h); glowline.cpp static_asserts they match.
static const uint8_t REASON_AUTH_EXPIRE = 2;
static const uint8_t REASON_MIC_FAILURE = 14;
static const uint8_t REASON_4WAY_HANDSHAKE_TIMEOUT = 15;
static const uint8_t REASON_AUTH_FAIL = 202;
static const uint8_t REASON_HANDSHAKE_TIMEOUT = 204;

// One STA disconnect reason: true when it counts as the saved network rejecting the password.
inline bool rejects(uint8_t reason) {
  switch (reason) {
    case REASON_AUTH_EXPIRE:
    case REASON_MIC_FAILURE:
    case REASON_4WAY_HANDSHAKE_TIMEOUT:
    case REASON_AUTH_FAIL:
    case REASON_HANDSHAKE_TIMEOUT:
      return true;
    default:
      return false;
  }
}
static const uint32_t SPAN_MS = 45000;

struct Watch {
  uint8_t fails = 0;
  uint32_t firstAt = 0;

  void associated() { fails = 0; }

  // One STA disconnect; true when the board should restart into setup mode.
  bool failed(bool wrongPassword, uint32_t now) {
    if (!wrongPassword) return false;
    if (fails == 0) firstAt = now;
    if (fails < 255) fails++;
    return fails >= FAILS && now - firstAt >= SPAN_MS;
  }
};

}  // namespace GlowlineWrongPw
