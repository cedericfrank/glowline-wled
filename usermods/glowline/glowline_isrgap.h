// SPDX-License-Identifier: EUPL-1.2
// Copyright (c) 2026 the Lumen authors. Part of a modified version of WLED; see NOTICE.md.
#pragma once
// BENCH PROBE ONLY (GLOWLINE_BENCH_ISR_GAP, REQ-165): finds what holds the S3's LED refill interrupt
// off mid-frame. Never in a shipped build.
//
// NeoPixelBus refills the RMT channel from an interrupt every 4 LEDs (120 us). If that interrupt is
// held off longer, the channel re-sends stale memory and the frame runs past the strip's set length
// (REQ-165). The RMT TX interrupt is allocated without ESP_INTR_FLAG_IRAM (esp-idf v5.5 rmt_private.h
// RMT_TX_INTR_ALLOC_FLAG; sdkconfig: CONFIG_RMT_TX_ISR_CACHE_SAFE unset), so it is masked whenever the
// flash cache is off (any flash write or erase, from either core) and whenever core 1 has interrupts
// off (a critical section) or is inside another level-1 handler.
//
// A gptimer interrupt is allocated the same way (sdkconfig: CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y,
// CONFIG_GPTIMER_ISR_CACHE_SAFE unset; Arduino's timerBegin() sets intr_shared) and from the loop
// task, so on core 1: it is held off by exactly the same things. This ticks it every 40 us and, when
// two ticks are further apart than 100 us, records the hold-off: how long, when, the interrupted
// code's address, the task, which part of WLED's loop ran, and how long after the last frame start.
//
// The interrupted address is read from the exception frame the interrupt vector pushed: on entry
// from a task, _frxt_int_enter (freertos portasm.S) stores the frame pointer in the task's TCB
// (pxTopOfStack, the TCB's first member, tasks.c), and the frame's `pc` field (xtensa_context.h
// XtExcFrame) is the return address, i.e. the instruction that was about to run when the interrupt
// finally got through: right after the critical section ended or the cache came back. Resolve it
// with addr2line on the build's ELF.
#include "wled.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "xtensa_context.h"
#include "esp_debug_helpers.h"
#include "esp_cpu.h"

namespace GlowlineIsrGap {

struct Gap {
  uint32_t us;        // the hold-off: time between two ticks, nominally 40
  uint32_t ms;        // millis() when it ended
  uint32_t pc;        // interrupted address (see above); 0 if unknown
  uint32_t sinceShow; // us since the last frame start; 0xFFFFFFFF before the first frame
  uint8_t phase;      // glowlineIsrGapPhase at the time (GlowlineLoopPhase)
  char task[12];
  char other[12];     // the task running on the other core at that moment (a flash op from core 0 shows there)
  uint32_t epc1;      // EPC1 as read in the handler (may be overwritten by a window exception on the way in)
  uint32_t w[6];      // the first words at the TCB's pxTopOfStack: exit, pc, ps, a0, a1, a2 if it is an XtExcFrame
  uint32_t bt[8];     // the interrupted task's return addresses, innermost first (esp_backtrace_get_next_frame)
};
static constexpr uint8_t BT = 14;

static constexpr uint32_t TICK_US = 40;
static constexpr uint32_t REPORT_US = 100;
static constexpr uint8_t RING = 6;

static hw_timer_t* timer = nullptr;
static volatile int64_t lastTick = 0;
static volatile int64_t lastShow = 0;
static volatile uint32_t frameUs = 0;      // how long a frame is on the line, from the strip length
static volatile bool measuring = false;    // the probe's own heap walk is running; don't record it
static Gap last[RING];                     // the last RING hold-offs, any time
static uint8_t lastIdx = 0, lastN = 0;
static Gap top[RING];                      // the RING longest hold-offs while a frame was on the line
static uint32_t ticks = 0, n100 = 0, n250 = 0, n1000 = 0, maxUs = 0;
static uint32_t in100 = 0, in250 = 0, in1000 = 0, inMaxUs = 0;
static uint32_t walkLast = 0, walkMax = 0, largest = 0;
static uint32_t blocksTotal = 0, blocksFree = 0;

static const char* phaseName(uint8_t p) {
  switch (p) {
    case GLP_TRANSITIONS: return "transitions";
    case GLP_USERMODS:    return "usermods";
    case GLP_CLOSEFILE:   return "closeFile";
    case GLP_PRESETS:     return "presets";
    case GLP_SERVICE:     return "service";
    case GLP_CFGWRITE:    return "cfgWrite";
    case GLP_WS:          return "ws";
    case GLP_WS_STATE:    return "wsState";
    case GLP_WS_LIVE:     return "wsLive";
    default:              return "other";
  }
}

static void IRAM_ATTR copyName(char* dst, size_t n, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i < n - 1; i++) dst[i] = src[i];
  dst[i] = '\0';
}

static void IRAM_ATTR tick() {
  const int64_t now = esp_timer_get_time();
  const int64_t prev = lastTick;
  lastTick = now;
  ticks++;
  if (!prev || measuring) return;
  const uint32_t gap = (uint32_t)(now - prev);
  if (gap < REPORT_US) return;

  Gap g;
  g.us = gap;
  g.ms = (uint32_t)(now / 1000);
  g.pc = 0;
  g.sinceShow = lastShow ? (uint32_t)(now - lastShow) : 0xFFFFFFFFu;
  g.phase = glowlineIsrGapPhase;
  g.task[0] = '\0';
  g.other[0] = '\0';
  for (uint8_t i = 0; i < BT; i++) g.bt[i] = 0;
  asm volatile("rsr.epc1 %0" : "=r"(g.epc1));
  for (uint8_t i = 0; i < 6; i++) g.w[i] = 0;
  TaskHandle_t t = xTaskGetCurrentTaskHandle();
  if (t) {
    const uint32_t* top = *(const uint32_t* const*)t; // TCB.pxTopOfStack = the interrupted frame
    for (uint8_t i = 0; i < 6; i++) g.w[i] = top[i];
    g.pc = top[1];
    // Walk the interrupted task's stack the way the panic handler does from an exception frame: the
    // vector spilled its register windows, so each frame's base save area holds the caller's a0/a1.
    esp_backtrace_frame_t fr = { .pc = top[1], .sp = top[4], .next_pc = top[3], .exc_frame = nullptr };
    for (uint8_t i = 0; i < BT; i++) g.bt[i] = 0;
    for (uint8_t i = 0; i < BT && fr.next_pc; i++) {
      if (!esp_backtrace_get_next_frame(&fr)) break;
      g.bt[i] = (uint32_t)esp_cpu_pc_to_addr(fr.pc);
    }
    copyName(g.task, sizeof(g.task), pcTaskGetName(t));
  }
  TaskHandle_t o = xTaskGetCurrentTaskHandleForCore(xPortGetCoreID() ? 0 : 1);
  if (o) copyName(g.other, sizeof(g.other), pcTaskGetName(o));

  n100++;
  if (gap >= 250) n250++;
  if (gap >= 1000) n1000++;
  if (gap > maxUs) maxUs = gap;
  last[lastIdx] = g;
  lastIdx = (lastIdx + 1) % RING;
  if (lastN < RING) lastN++;

  if (g.sinceShow < frameUs) {
    in100++;
    if (gap >= 250) in250++;
    if (gap >= 1000) in1000++;
    if (gap > inMaxUs) inMaxUs = gap;
    uint8_t mi = 0;
    for (uint8_t i = 1; i < RING; i++) if (top[i].us < top[mi].us) mi = i;
    if (gap > top[mi].us) top[mi] = g;
  }
}

// From the usermod's setup(): the loop task, core 1 (CONFIG_ARDUINO_RUNNING_CORE), so the timer's
// interrupt lands on the core the LED refill interrupt is on.
static void begin() {
  glowlineIsrGapLoopTask = xTaskGetCurrentTaskHandle();
  timer = timerBegin(1000000);
  if (!timer) { Serial.println(F("glowline isrgap: no timer")); return; }
  timerAttachInterrupt(timer, &tick);
  timerAlarm(timer, TICK_US, true, 0);
  Serial.printf("glowline isrgap: probe on, %lu us ticks, core %d\n", (unsigned long)TICK_US, xPortGetCoreID());
}

// From handleOverlayDraw(): right before the frame goes on the line.
static inline void stampShow() {
  lastShow = esp_timer_get_time();
}

// From the usermod's loop(), every 2 s and only while no frame is on the line (the walk is itself a
// hold-off: multi_heap_get_info_impl runs under portENTER_CRITICAL). Times what WLED's
// getContiguousFreeHeap() does on every segment copy, and counts the heap's blocks.
static void loopMeasure() {
  static unsigned long lastAt = 0;
  if (millis() - lastAt < 2000) return;
  if (strip.isUpdating()) return;
  lastAt = millis();
  frameUs = (uint32_t)strip.getLengthTotal() * 30 + 1000; // 30 us per RGB LED, plus the reset and slack
  measuring = true;
  const int64_t t0 = esp_timer_get_time();
  largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  measuring = false;
  walkLast = dt;
  if (dt > walkMax) walkMax = dt;
  blocksTotal = info.total_blocks;
  blocksFree = info.free_blocks;
}

static void addGap(JsonArray& arr, const Gap& g) {
  if (!g.us) return;
  JsonObject o = arr.createNestedObject();
  o["us"] = g.us;
  o["ms"] = g.ms;
  char pc[11];
  snprintf(pc, sizeof(pc), "0x%08lx", (unsigned long)g.pc);
  o["pc"] = pc;
  o["task"] = g.task;
  o["other"] = g.other;
  o["ph"] = phaseName(g.phase);
  JsonArray bt = o.createNestedArray("bt");
  for (uint8_t i = 0; i < BT && g.bt[i]; i++) { snprintf(pc, sizeof(pc), "0x%08lx", (unsigned long)g.bt[i]); bt.add(pc); }
  if (g.sinceShow != 0xFFFFFFFFu) o["show"] = g.sinceShow;
}

// GET /json/info -> lumen.probe
static void report(JsonObject& lumen) {
  JsonObject p = lumen.createNestedObject(F("probe"));
  p["on"] = timer != nullptr;
  p["up_s"] = millis() / 1000;
  p["ticks"] = ticks;
  p["frame_us"] = frameUs;
  p["n100"] = n100;  p["n250"] = n250;  p["n1000"] = n1000;  p["max_us"] = maxUs;
  p["in100"] = in100; p["in250"] = in250; p["in1000"] = in1000; p["in_max_us"] = inMaxUs;
  p["walk_us"] = walkLast; p["walk_max_us"] = walkMax; p["largest"] = largest;
  p["blocks"] = blocksTotal; p["free_blocks"] = blocksFree;
  JsonArray l = p.createNestedArray(F("last"));
  for (uint8_t i = 0; i < lastN; i++) addGap(l, last[(lastIdx + RING - lastN + i) % RING]);
  JsonArray t = p.createNestedArray(F("top"));
  for (uint8_t i = 0; i < RING; i++) addGap(t, top[i]);
}

} // namespace GlowlineIsrGap
