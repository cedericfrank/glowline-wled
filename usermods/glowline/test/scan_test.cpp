// Host test for the Wi-Fi list sent over Bluetooth (glowline_scan.h, REQ-135). Built and run by
// tools/glowline-host.test.mjs (npm test, Lumen CI).
#include "../glowline_scan.h"

#include <cstdio>
#include <cstring>

using namespace GlowlineScan;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static Net net(const char* ssid, int rssi, const char* auth) {
  Net n;
  strncpy(n.ssid, ssid, sizeof(n.ssid) - 1);
  n.ssid[sizeof(n.ssid) - 1] = 0;
  n.rssi = rssi;
  n.auth = auth;
  return n;
}

static void cleanSortsMergesAndDrops() {
  Net nets[] = {
    net("Office", -70, "WPA2"),
    net("", -40, "WPA2"),               // hidden
    net("Home", -60, "WPA2"),
    net("Corp", -45, nullptr),          // enterprise: can't join
    net("Home", -50, "WPA2/WPA3"),      // second access point, stronger
    net("Cafe", -80, "NO"),
    net("Home", -65, "WPA2"),           // third, weaker
  };
  size_t n = clean(nets, sizeof(nets) / sizeof(nets[0]));
  CHECK(n == 3);
  CHECK(!strcmp(nets[0].ssid, "Home") && nets[0].rssi == -50 && !strcmp(nets[0].auth, "WPA2/WPA3"));
  CHECK(!strcmp(nets[1].ssid, "Office") && nets[1].rssi == -70);
  CHECK(!strcmp(nets[2].ssid, "Cafe") && !strcmp(nets[2].auth, "NO"));
}

static void cleanKeepsScanOrderOnTies() {
  Net nets[] = { net("A", -60, "WPA2"), net("B", -60, "WPA2"), net("C", -50, "WPA2") };
  CHECK(clean(nets, 3) == 3);
  CHECK(!strcmp(nets[0].ssid, "C") && !strcmp(nets[1].ssid, "A") && !strcmp(nets[2].ssid, "B"));
}

static void cleanNothing() {
  Net nets[] = { net("", -40, "WPA2") };
  CHECK(clean(nets, 0) == 0);
  CHECK(clean(nets, 1) == 0);
}

static void packIsImprovStrings() {
  Net nets[] = { net("Home", -50, "WPA2"), net("Cafe", -80, "NO") };
  uint8_t out[255];
  size_t count = 99;
  size_t len = pack(nets, 2, out, sizeof(out), &count);
  const uint8_t want[] = { 4, 'H', 'o', 'm', 'e', 3, '-', '5', '0', 4, 'W', 'P', 'A', '2',
                           4, 'C', 'a', 'f', 'e', 3, '-', '8', '0', 2, 'N', 'O' };
  CHECK(count == 2);
  CHECK(len == sizeof(want) && !memcmp(out, want, sizeof(want)));
}

static void packStopsBeforeANetworkThatDoesNotFitWhole() {
  Net nets[] = { net("Home", -50, "WPA2"), net("Cafe", -80, "NO") };
  uint8_t out[255];
  size_t count = 0;
  CHECK(pack(nets, 2, out, 14, &count) == 14 && count == 1);  // exactly the first one
  CHECK(pack(nets, 2, out, 25, &count) == 14 && count == 1);  // one byte short of the second
  CHECK(pack(nets, 2, out, 13, &count) == 0 && count == 0);   // an empty result: "no network found"
}

static void packFillsAFullResultWithLongNames() {
  // 32-byte names: 1+32 + 1+3 + 1+4 = 42 bytes each, so 6 fit in 255 (252 bytes), the 7th doesn't.
  Net nets[10];
  for (int i = 0; i < 10; i++) {
    char name[33];
    std::snprintf(name, sizeof(name), "%02dxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
    nets[i] = net(name, -40 - i, "WPA2");
  }
  uint8_t out[255];
  size_t count = 0;
  CHECK(pack(nets, 10, out, sizeof(out), &count) == 252 && count == 6);
}

int main() {
  cleanSortsMergesAndDrops();
  cleanKeepsScanOrderOnTies();
  cleanNothing();
  packIsImprovStrings();
  packStopsBeforeANetworkThatDoesNotFitWhole();
  packFillsAFullResultWithLongNames();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
