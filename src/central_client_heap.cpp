// central_client_heap.cpp — heap-pressure / proactive-restart / trajectory
// methods of CentralClient.
//
// Split out of central_client.cpp in 0.2.40 (refactor pass) because this
// surface has been the bug-magnet of 0.2.x — BUG-077 / BUG-081 / BUG-082
// / BUG-084 / BUG-085 all lived in maybeHeapPressureRestart() and its
// adjacent helpers (compact-heartbeat hysteresis, trajectory
// discriminator). Co-locating them isolates the churn so future fixes
// land in one focused file instead of inflating the 1700+ LOC
// central_client.cpp diffs.
//
// All methods here are declared on CentralClient in central_client.h.
// C++ allows a single class to be defined across multiple translation
// units; PlatformIO compiles every .cpp under src/ and the linker
// stitches them. The resulting firmware.bin is byte-equivalent to the
// pre-split build modulo a few bytes of layout noise.
//
// File-scope (anonymous namespace) constants here are referenced only
// by the methods in this file. Class-level constants like HEAP_RING_SIZE
// and HEAP_SAMPLE_INTERVAL_MS still live in central_client.h and are
// accessible from any .cpp of the class.

#include <Arduino.h>
#include <ArduinoJson.h>
#include "central_client.h"
#include "app_state.h"
#include "config_manager.h"
#include "event_log.h"
#include "safe_restart.h"

namespace {

// #172 / 0.2.14: proactive planned-restart thresholds. mfb is the largest
// contiguous free block — BearSSL needs ~5-7K contiguous for _sc and
// failure inside the SDK presents as system_restart_local() (ghost reboot
// with empty restart reason) or REASON_EXCEPTION_RST (.185 at 23:17:31
// UTC). Restart proactively once mfb stays below the threshold for the
// debounce window AND the device has been up long enough that we're sure
// we're not bouncing on startup pressure. Both conditions must hold.
// 0.2.30: raised from 10000 → 11000. Live diag data from 2026-06-11
// showed devices SUSTAINING mfb=10080 frag=38% for 2+ minutes — just
// barely above the 10K floor so proactive restart didn't fire — then
// .188 brown-outed (REASON_DEFAULT_RST "Power On") inside that
// danger window. The heavy BearSSL retry traffic at sub-12K mfb
// causes a current spike the marginal power supply can't sustain.
// Setting the floor at 11K catches steady-state pressure BEFORE the
// brown-out, while leaving 1K margin above the ~10K-and-shrinking
// pattern that's been the chronic cascade trigger. The proactive
// restart path is now busy-wait + wdtFeed (0.2.26 fix) so firing
// more often no longer produces Exception-class secondary crashes.
// 0.2.33: bumped to 13000 from 11000. .185 was sitting stably at
// mfb=11216 frag=33-40% for ~80 seconds then crashing inside the
// WiFi SDK (ieee80211_setup_ratetable on the stack, EPC1=0x4000df64
// = ROM strncmp + 4 with NULL). Confirmed via 0.2.25 CrashRecorder +
// diag-syslog — the crash is the WiFi stack's internal allocations
// failing under heap fragmentation we can't reach from firmware code.
// 13K threshold + 15s debounce (was 30s) restarts the device BEFORE
// the WiFi stack panics. The original 0.2.22 13K problem (restart
// loops) is gone because the post-0.2.31/0.2.32 fixes mean each
// proactive restart is a clean Software/System restart instead of
// cascading into Exception-class secondary crashes.
constexpr uint16_t HEAP_PRESSURE_MFB_THRESHOLD = 13000;
// 0.2.33: 3 samples × 5s = 15s sustained (was 6×5s=30s). The WiFi
// SDK starts crashing within ~80s of sustained sub-13K mfb; the
// 30s debounce was leaving a 45s window for the WiFi-stack NULL
// deref to fire. 15s gives enough confidence the pressure isn't
// transient while still beating the SDK's crash timing.
constexpr uint8_t HEAP_PRESSURE_DEBOUNCE_SAMPLES = 3;
constexpr uint32_t HEAP_PRESSURE_CHECK_INTERVAL_MS = 5000;
constexpr uint32_t HEAP_PRESSURE_MIN_UPTIME_S = 1800;  // don't fire in the first 30min
// 0.2.34 BUG-077 fix (b): trajectory-slope discriminator. After the
// debounce window confirms mfb is below threshold, also look at the
// heap-trajectory ring. Only an OBSERVED DROP of ≥ this many bytes
// over the 60s window indicates real erosion that the proactive
// restart is meant to catch. See heapTrajectoryRecovering().
constexpr uint32_t HEAP_PRESSURE_RECOVERY_DELTA = 1024;
// 0.2.17 sweep S6: heap samples taken within this window after an HTTPS
// call returns are biased by the BearSSL buffer's transient post-alloc
// state and are skipped to avoid false-positive heap-pressure trips.
constexpr uint32_t HTTPS_SETTLE_WINDOW_MS = 1500;
constexpr uint32_t COMPACT_HEARTBEAT_FREE_HEAP_THRESHOLD = 20000;
// 0.2.34 BUG-077 fix (a): also drop to compact heartbeat when mfb is
// low, even if free_heap is healthy. .190's burst-loop trajectory
// shows fh hovering 20-21K (above the fh threshold so compact does
// NOT engage) while mfb oscillates 8-15K (well into the proactive
// danger zone). The verbose heartbeat then allocates a multi-K JSON
// + body String through that already-fragmented heap and re-fires
// the dip. Compact-on-fragmentation lets the device shed the
// extra alloc footprint on the cycles where it matters most.
constexpr uint32_t COMPACT_HEARTBEAT_MFB_THRESHOLD = 14000;
// 0.2.37 BUG-081: hysteresis so the heartbeat doesn't flip-flop
// between compact and verbose every cycle when fh / mfb sit near
// either threshold. Verbose peaks slice mfb; compact lows let fh
// recover above 20K; next cycle goes verbose again; loop. Adding
// an exit gap (1024 bytes on each metric) breaks the oscillation —
// the device stays compact until BOTH fh AND mfb climb a healthy
// margin above the enter thresholds.
constexpr uint32_t COMPACT_HEARTBEAT_FREE_HEAP_EXIT = 21024;
constexpr uint32_t COMPACT_HEARTBEAT_MFB_EXIT = 15024;

}  // namespace

bool CentralClient::shouldUseCompactHeartbeat() const {
  // 0.2.34 BUG-077 fix (a): trigger on EITHER low free-heap OR low
  // max-free-block. The original fh-only gate misses the .190
  // fragmented-but-not-low scenario (fh=21K, mfb=9K).
  // 0.2.37 BUG-081 fix: add hysteresis to prevent mode oscillation.
  // Once compact, stay compact until BOTH fh AND mfb climb a healthy
  // margin above the enter thresholds (the *_EXIT constants).
  const uint32_t fh = ESP.getFreeHeap();
  const uint32_t mfb = ESP.getMaxFreeBlockSize();
  if (compactHeartbeatLatched_) {
    if (fh >= COMPACT_HEARTBEAT_FREE_HEAP_EXIT &&
        mfb >= COMPACT_HEARTBEAT_MFB_EXIT) {
      compactHeartbeatLatched_ = false;
    }
  } else {
    if (fh < COMPACT_HEARTBEAT_FREE_HEAP_THRESHOLD ||
        mfb < COMPACT_HEARTBEAT_MFB_THRESHOLD) {
      compactHeartbeatLatched_ = true;
    }
  }
  return compactHeartbeatLatched_;
}

void CentralClient::maybeHeapPressureRestart() {
  if (!status_ || !cfgMgr_) return;
  const uint32_t now = millis();
  if (now - lastHeapPressureCheckMs_ < HEAP_PRESSURE_CHECK_INTERVAL_MS) return;
  lastHeapPressureCheckMs_ = now;

  // Don't bounce during startup or recovery — give the device room to
  // settle and let an operator clear a stuck state without us looping.
  if (status_->uptimeSeconds < HEAP_PRESSURE_MIN_UPTIME_S) return;
  if (status_->recoveryMode) return;

  // 0.2.38 BUG-085 fix: post-proactive-restart burst suppressor.
  // .190 data captured 2026-06-20: device fires proactive every ~30
  // min — exactly the HEAP_PRESSURE_MIN_UPTIME_S gate. The mfb is
  // sustained sub-13K from boot; the OUTER debounce passes the moment
  // the gate clears; the discriminator correctly sees real erosion
  // (single-shot drops from baseline) and fires. Result: 19 reboots
  // in 24h on a device that hasn't actually crashed WiFi-SDK once.
  //
  // The fix: if the PRIOR boot was already a proactive restart, the
  // problem isn't acute — the device just recovered from the same
  // pressure pattern. Give it 4 hours of runtime to either prove
  // it's safe (no WiFi-SDK crash → cap fires at 6/24h) or to fail
  // naturally (Exception/WDT reset captured via CrashRecorder, fresh
  // boot, fresh proactive eligibility). Caps the worst-case burst
  // cadence at one fire every ~4.5h instead of every ~30 min.
  //
  // Note: this does NOT reduce protection against the original
  // .185 WiFi-SDK NULL-deref scenario. That bug fired within 80s of
  // sustained sub-13K mfb. The FIRST proactive after any boot still
  // fires after 30 min of pressure — 80s vs 30 min, the protection
  // wins by a factor of 22.
  constexpr uint32_t PROACTIVE_BURST_SUPPRESS_AFTER_MS = 4UL * 60UL * 60UL * 1000UL;
  const bool priorWasProactive =
      status_->lastPlannedRestartReason == "heap_pressure_proactive";
  if (priorWasProactive &&
      static_cast<uint32_t>(status_->uptimeSeconds) * 1000UL < PROACTIVE_BURST_SUPPRESS_AFTER_MS) {
    return;
  }

  const uint32_t mfb = ESP.getMaxFreeBlockSize();
  if (mfb >= HEAP_PRESSURE_MFB_THRESHOLD) {
    heapPressureSampleCount_ = 0;
    return;
  }
  if (heapPressureSampleCount_ < HEAP_PRESSURE_DEBOUNCE_SAMPLES) {
    heapPressureSampleCount_++;
    return;
  }
  // 0.2.16 review fix #6: dropped the `if (!pendingCommandsJson_.isEmpty()) return`
  // guard. loop() ordering called this BEFORE the deferred-drain block, so the
  // check saw stale data from the previous iteration. With the drain now
  // re-ordered to run first, the buffer is already empty by the time we get
  // here in the common case. And even if it isn't: the hub queue is idempotent,
  // a command re-delivered after the restart still executes correctly, and
  // silently suppressing the safety net was the more dangerous failure mode.

  // 0.2.34 BUG-077 fix (b): if the heap trajectory shows recovery in
  // progress, the pressure is fragmentation noise resolving on its own
  // — suppress this fire. Reset the debounce so the NEXT below-threshold
  // run has to re-accumulate; otherwise we'd fire on the very next tick
  // as soon as recovery stops.
  if (heapTrajectoryRecovering()) {
    heapPressureSampleCount_ = 0;
    return;
  }

  // Sustained pressure with no recovery trend — fire a clean planned restart.
  Serial.print("Heap-pressure proactive restart: mfb=");
  Serial.print(mfb);
  Serial.print(" sustained for ");
  Serial.print(heapPressureSampleCount_ * HEAP_PRESSURE_CHECK_INTERVAL_MS / 1000);
  Serial.println("s");
  if (eventLog_) {
    eventLog_->add("system",
                   "Proactive restart: mfb=" + String(mfb) +
                       " below threshold, uptime=" + String(status_->uptimeSeconds) + "s");
    eventLog_->flush();
  }
  cfgMgr_->prepareForPlannedRestart("heap_pressure_proactive");
  // 0.2.26 CRITICAL fix (code review F6) — see safeRestartWait() in
  // include/safe_restart.h for the SYS-yield-race rationale. 0.2.33
  // factored this into a shared helper used by every pre-restart site.
  safeRestartWait(500);
  ESP.restart();
}

// 0.2.34 BUG-077 fix (b) — REVISED in 0.2.36 BUG-084:
// the original semantic ("return true when mfb has trended UP by
// >=1024B") had a subtle bug: a FLAT mfb (newest == oldest, stuck
// at e.g. 12456 for 60s) returned false → proactive restart fired
// → burst loop continued exactly the way BUG-077 documented.
//
// The corrected semantic: "return true when mfb is NOT actively
// eroding." A flat or recovering trajectory is post-fragmentation
// steady state, not danger. Only an OBSERVED DROP of >=1024B over
// the 60s window indicates real erosion that the proactive restart
// is meant to catch. Stable-low mfb without erosion is the .190
// fragmented-boot pattern that 0.2.34 was supposed to solve.
//
// .185-style protection (WiFi-SDK NULL-deref under sustained
// pressure) is still provided by the 13K threshold + 15s debounce;
// the discriminator only suppresses the burst pattern where mfb
// has been flat for the full ring window with no observed
// erosion.
//
// Returns TRUE when "not eroding" — caller suppresses fire.
// Returns FALSE when erosion observed — caller fires as before.
// Requires the ring to have at least 6 samples (30s window) —
// fewer samples can't reliably distinguish trend from noise; the
// 30-min uptime gate is the upstream protection so 30s of
// trajectory data is always available at fire time.
bool CentralClient::heapTrajectoryRecovering() const {
  if (heapRingCount_ < 6) return false;
  const uint8_t oldest = (heapRingHead_ + HEAP_RING_SIZE - heapRingCount_) % HEAP_RING_SIZE;
  const uint8_t newest = (heapRingHead_ + HEAP_RING_SIZE - 1) % HEAP_RING_SIZE;
  const uint16_t oldMfb = heapRing_[oldest].max_free_block;
  const uint16_t newMfb = heapRing_[newest].max_free_block;
  // Drop = oldest - newest. Positive when mfb fell across the window.
  if (newMfb >= oldMfb) {
    // Flat or recovering → not eroding → suppress.
    return true;
  }
  const uint32_t drop = static_cast<uint32_t>(oldMfb - newMfb);
  // Suppress unless the drop crosses the erosion threshold.
  return drop < HEAP_PRESSURE_RECOVERY_DELTA;
}

void CentralClient::sampleHeap() {
  const uint32_t now = millis();
  if (lastHeapSampleAtMs_ != 0 && now - lastHeapSampleAtMs_ < HEAP_SAMPLE_INTERVAL_MS) return;
  // 0.2.17 sweep S6: skip sampling inside the post-HTTPS settle window.
  // During the ~12K BearSSL handshake, ESP.getMaxFreeBlockSize() reports
  // the transient post-alloc value, not the steady-state baseline. If
  // the heap-pressure debounce (6 samples) catches us on a burst of
  // HTTPS calls — one sample per loop tick right after each return —
  // it would falsely trip the proactive restart on devices whose
  // BASELINE mfb is healthy. lastHttpsCompletedAtMs_ is bumped by
  // postWithFallback/getWithFallback when they finish (still in
  // central_client.cpp — those methods didn't move in this refactor).
  if (lastHttpsCompletedAtMs_ != 0 && now - lastHttpsCompletedAtMs_ < HTTPS_SETTLE_WINDOW_MS) return;
  lastHeapSampleAtMs_ = now;
  HeapSample& s = heapRing_[heapRingHead_];
  s.uptime_s = status_ ? status_->uptimeSeconds : (now / 1000UL);
  s.free_heap = static_cast<uint16_t>(min<uint32_t>(ESP.getFreeHeap(), 65535));
  s.max_free_block = static_cast<uint16_t>(min<uint32_t>(ESP.getMaxFreeBlockSize(), 65535));
  s.frag_pct = static_cast<uint8_t>(min<uint32_t>(ESP.getHeapFragmentation(), 255));
  heapRingHead_ = (heapRingHead_ + 1) % HEAP_RING_SIZE;
  if (heapRingCount_ < HEAP_RING_SIZE) heapRingCount_++;
}

void CentralClient::serializeHeapTrajectory(JsonDocument& doc) {
  if (heapRingCount_ == 0) return;
  JsonArray arr = doc["heap_trajectory"].to<JsonArray>();
  const uint8_t start = (heapRingHead_ + HEAP_RING_SIZE - heapRingCount_) % HEAP_RING_SIZE;
  for (uint8_t i = 0; i < heapRingCount_; ++i) {
    const HeapSample& s = heapRing_[(start + i) % HEAP_RING_SIZE];
    JsonObject e = arr.add<JsonObject>();
    e["up"] = s.uptime_s;
    e["fh"] = s.free_heap;
    e["mfb"] = s.max_free_block;
    e["fp"] = s.frag_pct;
  }
  // 0.2.17 sweep S8: reset the ring after flushing. Without this, the
  // 12-slot × 5s ring fills in ~60s — the same cadence as heartbeats —
  // so the next heartbeat re-ships up to all 12 samples it already
  // shipped (different received_at, same `up` values). Hub stored each
  // sample 2-3x and any chart that flattens trajectories by `up` showed
  // duplicate points. New samples are appended from the head; next
  // heartbeat ships only what was captured in the intervening window.
  heapRingCount_ = 0;
  heapRingHead_ = 0;
}
