// Host test for the back-to-setup-mode trigger (glowline_wrongpw.h, REQ-135). Built and run by
// tools/glowline-host.test.mjs (npm test, Lumen CI).
#include "../glowline_wrongpw.h"

#include <cstdio>

using namespace GlowlineWrongPw;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void threeRejectionsOverTheSpanFire() {
  Watch w;
  CHECK(!w.failed(true, 1000));
  CHECK(!w.failed(true, 19000));
  CHECK(!w.failed(true, 37000));          // 3 rejections, but only 36 s apart
  CHECK(w.failed(true, 1000 + SPAN_MS));  // the 4th, at the span
}

static void quickRejectionsAloneDoNotFire() {
  // The core's own auto-reconnect can reject several times within seconds: the span still applies.
  Watch w;
  for (uint32_t t = 0; t < 10000; t += 1000) CHECK(!w.failed(true, t));
}

static void routerOffNeverFires() {
  Watch w;
  for (uint32_t t = 0; t < 10UL * 60UL * 1000UL; t += 18000) CHECK(!w.failed(false, t));
}

static void scanMissesNeitherCountNorReset() {
  Watch w;
  CHECK(!w.failed(true, 0));
  CHECK(!w.failed(false, 18000));  // network not found between rejections (bench, REQ-131)
  CHECK(!w.failed(true, 36000));
  CHECK(!w.failed(false, 40000));
  CHECK(w.failed(true, 54000));
}

static void anAssociationResets() {
  Watch w;
  CHECK(!w.failed(true, 0));
  CHECK(!w.failed(true, 18000));
  w.associated();
  CHECK(!w.failed(true, 50000));   // counts from here again
  CHECK(!w.failed(true, 68000));
  CHECK(!w.failed(true, 86000));   // 3, only 36 s apart
  CHECK(w.failed(true, 95000));
}

static void countDoesNotWrap() {
  Watch w;
  for (int i = 0; i < 300; i++) w.failed(true, 0);
  CHECK(w.failed(true, SPAN_MS));
}

static void rejectionCodes() {
  CHECK(rejects(15) && rejects(204) && rejects(202) && rejects(14));  // wrong password, WPA2 and WPA3
  CHECK(rejects(2));                                                   // AUTH_EXPIRE: an iPhone hotspot's
  CHECK(!rejects(201) && !rejects(36) && !rejects(200) && !rejects(205) && !rejects(1) && !rejects(4));
}

static void iphoneHotspotWithAChangedPasswordFires() {
  // Bench 2026-11-05 (REQ-135 C): after the hotspot's password changed, only AUTH_EXPIRE (2), STA_LEAVING
  // (36) and NO_AP_FOUND (201), a few seconds apart. Without AUTH_EXPIRE counting, this never fired.
  const uint8_t seen[] = { 2, 201, 36, 2, 201, 36, 2, 2, 201, 36, 2, 36, 2, 2, 2 };
  Watch w;
  bool fired = false;
  uint32_t t = 0;
  for (uint8_t r : seen) {
    if (w.failed(rejects(r), t)) { fired = true; break; }
    t += 4000;
  }
  CHECK(fired && t >= SPAN_MS);
}

int main() {
  rejectionCodes();
  iphoneHotspotWithAChangedPasswordFires();
  threeRejectionsOverTheSpanFire();
  quickRejectionsAloneDoNotFire();
  routerOffNeverFires();
  scanMissesNeitherCountNorReset();
  anAssociationResets();
  countDoesNotWrap();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
