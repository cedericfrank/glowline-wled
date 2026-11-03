// Host test for the boot cue (glowline_cue.h, REQ-140). Built and run by
// tools/glowline-host.test.mjs (npm test, Lumen CI).
#include "../glowline_cue.h"

#include <cstdio>

using namespace GlowlineCue;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void darkFirstThenFill() {
  CHECK(next(Phase::CUE_SETTLE, 0, true, true) == Phase::CUE_SETTLE);
  CHECK(next(Phase::CUE_SETTLE, SETTLE_MS - 1, true, true) == Phase::CUE_SETTLE); // even if Wi-Fi is up already
  CHECK(next(Phase::CUE_SETTLE, SETTLE_MS, false, false) == Phase::CUE_FILL);
  CHECK(colourAt(Phase::CUE_SETTLE, 500, 0, 0, 480) == 0);
}

static void noWifiSavedIsOrangeNeverRed() {
  CHECK(next(Phase::CUE_FILL, FILL_MS - 1, false, false) == Phase::CUE_FILL);
  CHECK(next(Phase::CUE_FILL, FILL_MS, false, false) == Phase::CUE_ORANGE);
  CHECK(next(Phase::CUE_ORANGE, ORANGE_MS - 1, false, false) == Phase::CUE_ORANGE);
  CHECK(next(Phase::CUE_ORANGE, ORANGE_MS, false, false) == Phase::CUE_DONE);
  CHECK(colourAt(Phase::CUE_ORANGE, 0, 3000, 0, 480) == COLOUR_ORANGE);
  CHECK(COLOUR_ORANGE != COLOUR_RED && COLOUR_ORANGE != COLOUR_WHITE);
}

static void wifiSavedConnectsIsGreen() {
  CHECK(next(Phase::CUE_FILL, FILL_MS, true, false) == Phase::CUE_WAIT_WIFI);
  CHECK(next(Phase::CUE_FILL, FILL_MS, true, true) == Phase::CUE_WAIT_WIFI); // the fill always finishes first
  CHECK(next(Phase::CUE_WAIT_WIFI, 0, true, true) == Phase::CUE_GREEN);
  CHECK(next(Phase::CUE_WAIT_WIFI, 3000, true, false) == Phase::CUE_WAIT_WIFI);
  CHECK(next(Phase::CUE_GREEN, GREEN_MS - 1, true, true) == Phase::CUE_GREEN);
  CHECK(next(Phase::CUE_GREEN, GREEN_MS, true, true) == Phase::CUE_DONE);
  CHECK(colourAt(Phase::CUE_GREEN, 0, 5000, 7, 480) == COLOUR_GREEN);
}

static void wifiSavedTimesOutIsRed() {
  CHECK(next(Phase::CUE_WAIT_WIFI, WIFI_WAIT_MS - 1, true, false) == Phase::CUE_WAIT_WIFI);
  CHECK(next(Phase::CUE_WAIT_WIFI, WIFI_WAIT_MS, true, false) == Phase::CUE_RED);
  CHECK(next(Phase::CUE_RED, RED_MS - 1, true, false) == Phase::CUE_RED);
  CHECK(next(Phase::CUE_RED, RED_MS, true, false) == Phase::CUE_DONE);
  CHECK(colourAt(Phase::CUE_RED, 0, 12000, 0, 480) == COLOUR_RED);
}

static void fillGrowsAndFadesIn() {
  CHECK(colourAt(Phase::CUE_FILL, 0, 0, 0, 480) == 0);              // nothing lit yet
  CHECK(colourAt(Phase::CUE_FILL, 500, 500, 239, 480) != 0);        // first half lit...
  CHECK(colourAt(Phase::CUE_FILL, 500, 500, 240, 480) == 0);        // ...second half not yet
  CHECK(colourAt(Phase::CUE_FILL, 500, 500, 0, 480) == 0x555555);   // a third of the way faded in
  CHECK(colourAt(Phase::CUE_FILL, FILL_MS, FILL_MS, 479, 480) == scaled(COLOUR_WHITE, fadeIn(FILL_MS)));
  CHECK(colourAt(Phase::CUE_WAIT_WIFI, 0, FILL_MS, 0, 480) == 0xAAAAAA); // still fading after the fill
  CHECK(colourAt(Phase::CUE_WAIT_WIFI, 500, FADE_MS, 0, 480) == COLOUR_WHITE);
  CHECK(fadeIn(0) == 0 && fadeIn(FADE_MS) == 255 && fadeIn(60000) == 255);
}

static void doneDrawsNothing() {
  CHECK(next(Phase::CUE_DONE, 0, true, true) == Phase::CUE_DONE);
  CHECK(colourAt(Phase::CUE_DONE, 0, 0, 0, 480) == 0);
}

int main() {
  darkFirstThenFill();
  noWifiSavedIsOrangeNeverRed();
  wifiSavedConnectsIsGreen();
  wifiSavedTimesOutIsRed();
  fillGrowsAndFadesIn();
  doneDrawsNothing();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("cue: all checks passed\n");
  return 0;
}
