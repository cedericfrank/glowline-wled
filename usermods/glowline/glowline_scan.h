#pragma once
// The Wi-Fi list a phone asks for over Bluetooth (Improv 0x04, REQ-135, lumen-shared contracts/firmware.md
// "BLE provisioning (Improv)"): what the board's scan found, cleaned up and packed into one Improv result.
//   - hidden networks (no name) and ones the board can't join (security name nullptr) are left out
//   - a name seen on several access points is listed once, at its strongest signal
//   - strongest first; as many as fit in one result (Improv BLE defines exactly one, no end marker)
//
// Pure logic only, tested on the host: usermods/glowline/test/scan_test.cpp (npm test).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace GlowlineScan {

struct Net {
  char ssid[33];     // NUL-terminated, "" = hidden
  int rssi;          // dBm
  const char* auth;  // Improv's security name ("WPA2", "WPA/WPA2", "NO", ...); nullptr = can't join, left out
};

// Cleans `nets` in place: drops hidden and unjoinable networks, merges names seen more than once into
// the strongest, sorts strongest first (ties keep scan order). Returns how many are left.
inline size_t clean(Net* nets, size_t n) {
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    if (nets[i].ssid[0] == 0 || nets[i].auth == nullptr) continue;
    size_t j = 0;
    while (j < kept && strcmp(nets[j].ssid, nets[i].ssid) != 0) j++;
    if (j < kept) {
      if (nets[i].rssi > nets[j].rssi) nets[j] = nets[i];
      continue;
    }
    nets[kept++] = nets[i];
  }
  for (size_t i = 1; i < kept; i++) {  // insertion sort: stable, and a scan lists at most a few dozen
    Net x = nets[i];
    size_t j = i;
    while (j > 0 && nets[j - 1].rssi < x.rssi) { nets[j] = nets[j - 1]; j--; }
    nets[j] = x;
  }
  return kept;
}

// Packs the first networks as Improv result strings (length, bytes) x 3 per network: SSID, RSSI as
// decimal text, security name. Stops before the first network that would not fit whole in `maxLen`
// bytes. Returns the bytes written; `*count` = networks packed.
inline size_t pack(const Net* nets, size_t n, uint8_t* out, size_t maxLen, size_t* count) {
  size_t len = 0, packed = 0;
  for (size_t i = 0; i < n; i++) {
    char rssi[8];
    snprintf(rssi, sizeof(rssi), "%d", nets[i].rssi);
    const char* s[3] = { nets[i].ssid, rssi, nets[i].auth };
    size_t need = 0;
    for (const char* str : s) need += 1 + strlen(str);
    if (len + need > maxLen) break;
    for (const char* str : s) {
      size_t sl = strlen(str);
      out[len++] = (uint8_t)sl;
      memcpy(out + len, str, sl);
      len += sl;
    }
    packed++;
  }
  if (count) *count = packed;
  return len;
}

}  // namespace GlowlineScan
