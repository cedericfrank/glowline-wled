// Host test for the factory burn-in logic (glowline_burnin.h, REQ-092). No Arduino: built with the
// host compiler by tools/glowline-burnin.test.mjs, which `npm test` (and Lumen CI) runs.
#include "../glowline_burnin.h"

#include <cstdio>
#include <cstring>

using namespace GlowlineBurnIn;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static Record armed(uint16_t minutes) {
  Record r;
  r.state = State::ARMED;
  r.minutes = minutes;
  return r;
}

static void armedPowerUpRuns() {
  Boot b = onBoot(armed(30), Reset::POWER_CUT);
  CHECK(b.run);
  CHECK(!b.failed);
  CHECK(b.changed);
  CHECK(b.rec.state == State::RUNNING);
  CHECK(b.rec.elapsed == 0);
  CHECK(b.rec.minutes == 30);
}

static void passesAfterTheAskedMinutes() {
  Record r = onBoot(armed(3), Reset::POWER_CUT).rec;
  r = onMinute(r);
  CHECK(r.state == State::RUNNING && r.elapsed == 1);
  r = onMinute(r);
  CHECK(r.state == State::RUNNING && r.elapsed == 2);
  r = onMinute(r);
  CHECK(r.state == State::PASSED && r.elapsed == 3);
  CHECK(onMinute(r).elapsed == 3); // nothing counts after the result
}

static void brownoutFailsWithReasonAndMinute() {
  Record r = onBoot(armed(30), Reset::POWER_CUT).rec;
  for (int i = 0; i < 12; i++) r = onMinute(r);
  Boot b = onBoot(r, Reset::BROWNOUT);
  CHECK(!b.run);
  CHECK(b.failed);
  CHECK(b.changed);
  CHECK(b.rec.state == State::FAILED);
  CHECK(b.rec.reset == Reset::BROWNOUT);
  CHECK(b.rec.elapsed == 12);
}

static void anyRestartInTheFirstMinuteFailsExceptAPowerCut() {
  Record r = onBoot(armed(30), Reset::POWER_CUT).rec;
  Boot swap = onBoot(r, Reset::POWER_CUT); // USB -> barrel after it had started: start again
  CHECK(swap.run);
  CHECK(!swap.failed);
  CHECK(!swap.changed);
  CHECK(swap.rec.state == State::RUNNING && swap.rec.elapsed == 0);
  const Reset others[] = {Reset::BROWNOUT, Reset::POWER_GLITCH, Reset::CRASH, Reset::WATCHDOG, Reset::RESTART, Reset::OTHER};
  for (Reset why : others) {
    Boot b = onBoot(r, why);
    CHECK(!b.run && b.failed && b.rec.state == State::FAILED && b.rec.reset == why);
  }
}

static void powerCutAfterTheFirstMinuteFails() {
  Record r = onMinute(onBoot(armed(30), Reset::POWER_CUT).rec);
  Boot b = onBoot(r, Reset::POWER_CUT);
  CHECK(!b.run);
  CHECK(b.failed);
  CHECK(b.rec.state == State::FAILED && b.rec.reset == Reset::POWER_CUT && b.rec.elapsed == 1);
}

static void resultBootsAreNormalAndKeepTheResult() {
  Record passed;
  passed.state = State::PASSED;
  passed.minutes = passed.elapsed = 30;
  Record failed;
  failed.state = State::FAILED;
  failed.minutes = 30;
  failed.elapsed = 5;
  failed.reset = Reset::CRASH;
  const Record recs[] = {Record(), passed, failed};
  for (const Record& r : recs) {
    Boot b = onBoot(r, Reset::BROWNOUT);
    CHECK(!b.run && !b.failed && !b.changed);
    CHECK(b.rec.state == r.state && b.rec.elapsed == r.elapsed && b.rec.reset == r.reset);
  }
}

static void armingRules() {
  CHECK(std::strcmp(armRefusal(false, State::NONE, 30), "not_blank") == 0);
  CHECK(std::strcmp(armRefusal(true, State::RUNNING, 30), "running") == 0);
  CHECK(std::strcmp(armRefusal(true, State::NONE, 0), "bad_min") == 0);
  CHECK(std::strcmp(armRefusal(true, State::NONE, MAX_MINUTES + 1), "bad_min") == 0);
  CHECK(armRefusal(true, State::NONE, 30) == nullptr);
  CHECK(armRefusal(true, State::NONE, 1) == nullptr);
  CHECK(armRefusal(true, State::NONE, MAX_MINUTES) == nullptr);
  CHECK(armRefusal(true, State::ARMED, 30) == nullptr);  // armed again: the new minutes win
  CHECK(armRefusal(true, State::PASSED, 30) == nullptr); // a second run after a result
  CHECK(armRefusal(true, State::FAILED, 30) == nullptr);
}

static void provisioningWaitsForAPendingOrRunningBurnIn() {
  CHECK(blocksProvision(State::ARMED));
  CHECK(blocksProvision(State::RUNNING));
  CHECK(!blocksProvision(State::NONE));
  CHECK(!blocksProvision(State::PASSED));
  CHECK(!blocksProvision(State::FAILED));
}

static void coloursThenWhiteThenDoneMark() {
  CHECK(colourAt(0, false, 0) == 0xFF0000);
  CHECK(colourAt(COLOUR_STEP_MS - 1, false, 479) == 0xFF0000);
  CHECK(colourAt(COLOUR_STEP_MS, false, 0) == 0x00FF00);
  CHECK(colourAt(2 * COLOUR_STEP_MS, false, 0) == 0x0000FF);
  CHECK(colourAt(3 * COLOUR_STEP_MS, false, 0) == 0xFFFFFF);
  CHECK(colourAt(4 * COLOUR_STEP_MS, false, 0) == 0xFFFFFF);
  CHECK(colourAt(30u * 60u * 1000u, true, 0) == 0x00FF00);
  CHECK(colourAt(30u * 60u * 1000u, true, DONE_MARK_LEDS - 1) == 0x00FF00);
  CHECK(colourAt(30u * 60u * 1000u, true, DONE_MARK_LEDS) == 0xFFFFFF); // the load stays on
}

static void namesAndStoredBytes() {
  CHECK(std::strcmp(stateName(State::NONE), "none") == 0);
  CHECK(std::strcmp(stateName(State::ARMED), "armed") == 0);
  CHECK(std::strcmp(stateName(State::RUNNING), "running") == 0);
  CHECK(std::strcmp(stateName(State::PASSED), "passed") == 0);
  CHECK(std::strcmp(stateName(State::FAILED), "failed") == 0);
  CHECK(resetName(Reset::NONE) == nullptr);
  CHECK(std::strcmp(resetName(Reset::POWER_CUT), "power_cut") == 0);
  CHECK(std::strcmp(resetName(Reset::BROWNOUT), "brownout") == 0);
  CHECK(std::strcmp(resetName(Reset::POWER_GLITCH), "power_glitch") == 0);
  CHECK(std::strcmp(resetName(Reset::CRASH), "crash") == 0);
  CHECK(std::strcmp(resetName(Reset::WATCHDOG), "watchdog") == 0);
  CHECK(std::strcmp(resetName(Reset::RESTART), "restart") == 0);
  CHECK(std::strcmp(resetName(Reset::OTHER), "other") == 0);
  CHECK(stateFromByte(4) == State::FAILED);
  CHECK(stateFromByte(5) == State::NONE);
  CHECK(stateFromByte(0xFF) == State::NONE); // never written
  CHECK(resetFromByte(7) == Reset::OTHER);
  CHECK(resetFromByte(0xFF) == Reset::OTHER);
}

int main() {
  armedPowerUpRuns();
  passesAfterTheAskedMinutes();
  brownoutFailsWithReasonAndMinute();
  anyRestartInTheFirstMinuteFailsExceptAPowerCut();
  powerCutAfterTheFirstMinuteFails();
  resultBootsAreNormalAndKeepTheResult();
  armingRules();
  provisioningWaitsForAPendingOrRunningBurnIn();
  coloursThenWhiteThenDoneMark();
  namesAndStoredBytes();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("burn-in: all checks passed\n");
  return 0;
}
