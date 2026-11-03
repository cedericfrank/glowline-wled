#pragma once
// Factory burn-in (REQ-092, lumen-shared contracts/firmware.md "Factory burn-in"): the browser
// flasher arms it over serial while the board is on USB (strip dark: the strip is powered only from
// the barrel jack). The next power-up, on the product's own 5 V brick with USB unplugged, runs it:
// red, green, blue and white for the operator to see, then the whole strip white under the
// brightness limiter for the asked minutes. Nothing watches the board meanwhile, so it judges
// itself: any restart during the run fails it, with the chip's reset reason. The result stays in
// NVS until the page reads it and clears it (or provisioning erases it).
//
// Pure logic only (no Arduino, no NVS), so it is tested on the host:
// usermods/glowline/test/burnin_test.cpp, run by tools/glowline-host.test.mjs (npm test).

#include <stdint.h>

#include "glowline_cue.h" // the same gentle start as the boot cue

namespace GlowlineBurnIn {

enum class State : uint8_t { NONE = 0, ARMED = 1, RUNNING = 2, PASSED = 3, FAILED = 4 };

// Why a running burn-in restarted, from the chip's reset reason (glowline.cpp burnInResetFrom()).
enum class Reset : uint8_t { NONE = 0, POWER_CUT, BROWNOUT, POWER_GLITCH, CRASH, WATCHDOG, RESTART, OTHER };

static const uint16_t MAX_MINUTES = 240;
static const uint32_t COLOUR_STEP_MS = 3000; // red, green, blue, white: 3 s each, after 1 s dark
static const uint16_t DONE_MARK_LEDS = 10;   // green at the start of the strip once it has passed

struct Record {
  State state = State::NONE;
  uint16_t minutes = 0; // asked for
  uint16_t elapsed = 0; // whole minutes run so far (the minute of the restart, when failed)
  Reset reset = Reset::NONE;
};

struct Boot {
  Record rec;
  bool run;     // this boot runs the burn-in
  bool failed;  // this boot shows the failure (dim red) instead of the boot cue
  bool changed; // rec must be written back to NVS
};

// What a power-up does with the stored record.
inline Boot onBoot(Record r, Reset why) {
  switch (r.state) {
    case State::ARMED:
      r.state = State::RUNNING;
      r.elapsed = 0;
      r.reset = Reset::NONE;
      return {r, true, false, true};
    case State::RUNNING:
      // A power cut before the first whole minute is the operator moving the board from USB to the
      // barrel jack after it had already started (e.g. the port was closed and reopened, which
      // restarts the board): start again. Any other restart, or a cut later on, fails the run.
      if (why == Reset::POWER_CUT && r.elapsed == 0) return {r, true, false, false};
      r.state = State::FAILED;
      r.reset = why;
      return {r, false, true, true};
    default:
      return {r, false, false, false}; // NONE, PASSED, FAILED: a normal boot, the result is kept
  }
}

// Once per whole minute of a running boot.
inline Record onMinute(Record r) {
  if (r.state != State::RUNNING) return r;
  if (r.elapsed < 0xFFFF) r.elapsed++;
  if (r.elapsed >= r.minutes) r.state = State::PASSED;
  return r;
}

// nullptr = the board may be armed; else the reply's err code. `blank` is the serial provisioning
// rule (no device ID, token, passphrase or saved Wi-Fi): a set-up board never runs a burn-in.
inline const char* armRefusal(bool blank, State current, long minutes) {
  if (!blank) return "not_blank";
  if (current == State::RUNNING) return "running";
  if (minutes < 1 || minutes > MAX_MINUTES) return "bad_min";
  return nullptr;
}

// Serial provisioning waits until a burn-in is no longer pending or running, so a provisioned
// (shippable) board can never start one at its owner's first power-up.
inline bool blocksProvision(State s) { return s == State::ARMED || s == State::RUNNING; }

// Colour (0xRRGGBB) of LED i, ms after the run started; the limiter scales it down. Dark for the
// cue's settle time, then the colours fade in as the boot cue's white does (REQ-140): a full-strip
// step load right at power-up can brown out a good board on its own brick and fail it in minute 0.
inline uint32_t colourAt(uint32_t ms, bool passed, uint16_t i) {
  static const uint32_t steps[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
  if (ms < GlowlineCue::SETTLE_MS) return 0;
  uint32_t t = ms - GlowlineCue::SETTLE_MS;
  if (t < 4 * COLOUR_STEP_MS) return GlowlineCue::scaled(steps[t / COLOUR_STEP_MS], GlowlineCue::fadeIn(t));
  if (passed && i < DONE_MARK_LEDS) return 0x00FF00;
  return 0xFFFFFF;
}

inline const char* stateName(State s) {
  switch (s) {
    case State::ARMED:   return "armed";
    case State::RUNNING: return "running";
    case State::PASSED:  return "passed";
    case State::FAILED:  return "failed";
    default:             return "none";
  }
}

inline const char* resetName(Reset r) {
  switch (r) {
    case Reset::POWER_CUT:    return "power_cut";
    case Reset::BROWNOUT:     return "brownout";
    case Reset::POWER_GLITCH: return "power_glitch";
    case Reset::CRASH:        return "crash";
    case Reset::WATCHDOG:     return "watchdog";
    case Reset::RESTART:      return "restart";
    case Reset::OTHER:        return "other";
    default:                  return nullptr;
  }
}

// NVS stores raw bytes; anything unknown reads as no burn-in.
inline State stateFromByte(uint8_t b) { return b <= (uint8_t)State::FAILED ? (State)b : State::NONE; }
inline Reset resetFromByte(uint8_t b) { return b <= (uint8_t)Reset::OTHER ? (Reset)b : Reset::OTHER; }

} // namespace GlowlineBurnIn
