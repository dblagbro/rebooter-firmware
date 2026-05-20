#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

#include "time_sync_manager.h"

namespace {
static constexpr uint32_t RESYNC_ATTEMPT_INTERVAL_MS = 60000UL;
static constexpr time_t MIN_REASONABLE_EPOCH = 1704067200;  // 2024-01-01T00:00:00Z
}

void TimeSyncManager::begin(RuntimeStatus* status) {
  status_ = status;
  if (!status_) return;
  status_->timeSynced = false;
  status_->wallClockUnixMs = 0;
  status_->power.lastSampleUnixMs = 0;
}

void TimeSyncManager::loop(bool wifiConnected) {
  if (!status_) return;

  const uint32_t nowMs = millis();
  if (wifiConnected &&
      (!configured_ || (!status_->timeSynced && (nowMs - lastConfigAttemptAtMs_) >= RESYNC_ATTEMPT_INTERVAL_MS))) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    configured_ = true;
    lastConfigAttemptAtMs_ = nowMs;
  }

  refreshClockStatus_(nowMs);
}

void TimeSyncManager::refreshClockStatus_(uint32_t nowMs) {
  timeval tv;
  if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec < MIN_REASONABLE_EPOCH) {
    status_->timeSynced = false;
    status_->wallClockUnixMs = 0;
    status_->power.lastSampleUnixMs = 0;
    return;
  }

  const uint64_t nowUnixMs =
      (static_cast<uint64_t>(tv.tv_sec) * 1000ULL) +
      static_cast<uint64_t>(tv.tv_usec / 1000ULL);

  status_->timeSynced = true;
  status_->wallClockUnixMs = nowUnixMs;

  if (status_->power.lastSampleMillis > 0 && nowMs >= status_->power.lastSampleMillis) {
    const uint32_t ageMs = nowMs - status_->power.lastSampleMillis;
    status_->power.lastSampleUnixMs = (nowUnixMs >= ageMs) ? (nowUnixMs - ageMs) : 0;
  } else {
    status_->power.lastSampleUnixMs = 0;
  }
}
