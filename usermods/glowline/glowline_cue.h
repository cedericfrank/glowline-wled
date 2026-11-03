#pragma once
// Boot cue (REQ-140, lumen-shared contracts/firmware.md "Boot cue"): what the strip shows once per
// power-up, drawn by GlowlineUsermod::handleOverlayDraw() at the cue's 50 % brightness under the
// limiter, then handed back to the last view (or off) with no fade.
//   dark 1 s -> white fill over 1 s, fading in over 1.5 s ->
//     no Wi-Fi saved:            orange 2 s (waiting for setup)
//     Wi-Fi saved, connects:     white until it does, then green 1 s
//     Wi-Fi saved, 10 s go by:   red 5 s
// The dark second and the fade-in keep the 5 V brick from a ~2 A step right after it is plugged
// in, which restarted a board once or twice at LED 80-100 (Lumen-6ca4, 2026-11-03).
//
// Pure logic only, tested on the host: usermods/glowline/test/cue_test.cpp (npm test).

#include <stdint.h>

namespace GlowlineCue {

// Names kept from the first cue (CUE_*), which the usermod's other code compares against.
enum class Phase : uint8_t { CUE_SETTLE, CUE_FILL, CUE_WAIT_WIFI, CUE_GREEN, CUE_RED, CUE_ORANGE, CUE_DONE };

static const uint32_t SETTLE_MS = 1000;     // dark: the brick's output settles before any load
static const uint32_t FILL_MS = 1000;       // white grows from the first LED to the last
static const uint32_t FADE_MS = 1500;       // and fades in from 0 meanwhile (from the fill's start)
static const uint32_t WIFI_WAIT_MS = 10000; // after the fill, before red
static const uint32_t GREEN_MS = 1000;
static const uint32_t RED_MS = 5000;
static const uint32_t ORANGE_MS = 2000;

static const uint32_t COLOUR_WHITE = 0xFFFFFF;
static const uint32_t COLOUR_GREEN = 0x00FF00;
static const uint32_t COLOUR_RED = 0xFF0000;
// (COLOUR_*: WLED's FX.h defines RED, GREEN, WHITE, ORANGE as macros.)
static const uint32_t COLOUR_ORANGE = 0xFFA000; // 255,160,0: after WLED's colour gamma it still reads orange, not red

// The phase after `p`, `inPhase` ms after it began.
inline Phase next(Phase p, uint32_t inPhase, bool wifiSaved, bool wifiConnected) {
  switch (p) {
    case Phase::CUE_SETTLE:    return inPhase >= SETTLE_MS ? Phase::CUE_FILL : p;
    case Phase::CUE_FILL:
      if (inPhase < FILL_MS) return p;
      return wifiSaved ? Phase::CUE_WAIT_WIFI : Phase::CUE_ORANGE;
    case Phase::CUE_WAIT_WIFI:
      if (wifiConnected) return Phase::CUE_GREEN;
      return inPhase >= WIFI_WAIT_MS ? Phase::CUE_RED : p;
    case Phase::CUE_GREEN:     return inPhase >= GREEN_MS ? Phase::CUE_DONE : p;
    case Phase::CUE_RED:       return inPhase >= RED_MS ? Phase::CUE_DONE : p;
    case Phase::CUE_ORANGE:    return inPhase >= ORANGE_MS ? Phase::CUE_DONE : p;
    default:                   return Phase::CUE_DONE;
  }
}

// 0..255: how far the white has faded in, `sinceFill` ms after the fill began.
inline uint8_t fadeIn(uint32_t sinceFill) {
  return sinceFill >= FADE_MS ? 255 : (uint8_t)((sinceFill * 255) / FADE_MS);
}

inline uint32_t scaled(uint32_t rgb, uint8_t level) {
  uint32_t r = ((rgb >> 16) & 0xFF) * level / 255, g = ((rgb >> 8) & 0xFF) * level / 255, b = (rgb & 0xFF) * level / 255;
  return (r << 16) | (g << 8) | b;
}

// Colour (0xRRGGBB) of LED i of n; `sinceFill` is 0 before the fill began.
inline uint32_t colourAt(Phase p, uint32_t inPhase, uint32_t sinceFill, uint16_t i, uint16_t n) {
  switch (p) {
    case Phase::CUE_FILL: {
      uint32_t lit = (uint32_t)n * (inPhase < FILL_MS ? inPhase : FILL_MS) / FILL_MS;
      return i < lit ? scaled(COLOUR_WHITE, fadeIn(sinceFill)) : 0;
    }
    case Phase::CUE_WAIT_WIFI: return scaled(COLOUR_WHITE, fadeIn(sinceFill));
    case Phase::CUE_GREEN:     return COLOUR_GREEN;
    case Phase::CUE_RED:       return COLOUR_RED;
    case Phase::CUE_ORANGE:    return COLOUR_ORANGE;
    default:                   return 0; // CUE_SETTLE: dark; CUE_DONE: not drawn
  }
}

} // namespace GlowlineCue
