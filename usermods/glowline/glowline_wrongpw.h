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
// Pure logic only, tested on the host: usermods/glowline/test/wrongpw_test.cpp (npm test).

#include <stdint.h>

namespace GlowlineWrongPw {

static const uint8_t FAILS = 3;
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
