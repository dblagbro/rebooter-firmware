#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "diag_syslog.h"
#include "event_log.h"
#include "pre_crash_breadcrumb.h"

namespace {
constexpr uint32_t EVENT_LOG_PERSIST_DEBOUNCE_MS = 1500UL;
constexpr uint32_t EVENT_LOG_PERSIST_RETRY_MS = 5000UL;
constexpr uint32_t EVENT_LOG_MIN_FREE_HEAP = 12000UL;
constexpr uint32_t EVENT_LOG_BOOT_QUIET_PERIOD_MS = 600000UL;
}

void EventLog::begin(uint16_t maxEntries) {
  // 0.2.6: cap hard at 30 (was 100). items_ is a std::vector<EventEntry>
  // held in RAM for the device's lifetime + reloaded from /events.json at
  // boot. At the old 100-entry cap a fully-populated log held ~10-16K in
  // RAM (100 x two heap Strings), which eroded free heap from ~20K fresh
  // to ~10K — below the ~12-13K BearSSL needs for the hub TLS handshake —
  // and tipped devices into a hub-unreachable death spiral (confirmed on
  // .185 2026-05-28; factory-resetting /events.json restored heap to 22K).
  // The hub is the system-of-record for event history now (full event feed
  // + device.rebooted events); the device only needs a small recent buffer.
  // 30 entries caps the held RAM at ~3-5K and shrinks the boot-time
  // deserializeJson parse spike proportionally.
  maxEntries_ = max<uint16_t>(15, min<uint16_t>(maxEntries, 30));
  autoPersistAllowedAtMillis_ = millis() + EVENT_LOG_BOOT_QUIET_PERIOD_MS;
  load();
  trimToLimit();
  items_.reserve(maxEntries_);
  if (!items_.empty()) {
    const EventEntry& last = items_.back();
    nextSeq_ = max<uint32_t>(last.seq + 1, static_cast<uint32_t>(items_.size() + 1));
    bootId_ = max<uint32_t>(last.bootId + 1, 1);
  }
}

void EventLog::add(const String& type, const String& message) {
  // 0.2.27 #206: tap every event into the diag UDP syslog. No-op when
  // the syslog isn't configured. Fire-and-forget — never blocks. Done
  // BEFORE the heap-pressure bail below so we get the diag packet even
  // when we can't persist the entry locally.
  DiagSyslog::sendEvent(type, message);

  // 0.2.31 #208 (CRITICAL): bail out when heap is too low to safely
  // construct the String objects below. The Arduino String operations
  // (substring, concat, +, comparison via ==) can fail under heap
  // pressure with a NULL internal buffer. The comparison at the dedup
  // check (last.type == cleanType && last.message == cleanMessage) then
  // passes NULL to ROM strncmp at 0x40010DA0, exception cause 29.
  // Both .185 and .188 crash-dumped at the exact same address proving
  // the bug is deterministic at sub-12K heap, not a fleet-wide flake.
  // 4K is the safety floor below which we can't trust new String
  // allocations; the diag packet above still captures the event.
  if (ESP.getMaxFreeBlockSize() < 4000) return;

  String cleanType = type;
  cleanType.trim();
  if (cleanType.isEmpty()) cleanType = "event";
  if (cleanType.length() > 32) cleanType = cleanType.substring(0, 32);

  String cleanMessage = message;
  cleanMessage.trim();
  // 0.2.6: 80-char cap (was 160). Each stored message is a heap String held
  // in RAM; halving the cap halves the worst-case per-entry footprint. 80
  // covers the firmware's actual event strings (the longest in-tree is
  // ~45 chars) with margin.
  if (cleanMessage.length() > 80) {
    cleanMessage = cleanMessage.substring(0, 77) + "...";
  }
  // 0.2.31: additional defense — if either String came out empty after
  // trim+normalize, the source had no usable content; logging an empty
  // entry is pointless and adding it to the dedup ring confuses future
  // comparisons. Skip.
  if (cleanType.length() == 0 || cleanMessage.length() == 0) return;

  const uint32_t nowSeconds = millis() / 1000UL;
  if (!items_.empty()) {
    const EventEntry& last = items_.back();
    if (last.type == cleanType &&
        last.message == cleanMessage &&
        last.bootId == bootId_ &&
        nowSeconds >= last.ts &&
        (nowSeconds - last.ts) < suppressDuplicateWindowSeconds_) {
      return;
    }
  }

  items_.push_back({nextSeq_++, bootId_, nowSeconds, cleanType, cleanMessage});
  trimToLimit();
  dirty_ = true;
  lastMutationMillis_ = millis();
}

String EventLog::asJson() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& item : items_) {
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = item.seq;
    o["boot_id"] = item.bootId;
    o["ts"] = item.ts;
    o["ts_basis"] = "uptime_seconds";
    o["type"] = item.type;
    o["message"] = item.message;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void EventLog::trimToLimit() {
  while (items_.size() > maxEntries_) items_.erase(items_.begin());
}

void EventLog::load() {
  items_.clear();
  // 0.2.25 CRITICAL fix (code review F2): if a reboot landed in the
  // narrow window between persist()'s remove(logPath_) and
  // rename(tmp, logPath_), only the .tmp survives. Pre-fix `load()`
  // only checked logPath_, so the next persist() truncated tmp and
  // we lost the entire event log — exactly around the very crash we
  // wanted to investigate. Adopt-by-rename on boot before anything
  // else.
  const String tmpPath = String(logPath_) + ".tmp";
  if (!LittleFS.exists(logPath_) && LittleFS.exists(tmpPath)) {
    LittleFS.rename(tmpPath, logPath_);
  }
  if (!LittleFS.exists(logPath_)) {
    if (LittleFS.exists(tmpPath)) LittleFS.remove(tmpPath);  // stale empty tmp
    return;
  }

  File f = LittleFS.open(logPath_, "r");
  if (!f) return;

  // Defensive guard: if the persisted event log grows unexpectedly large or
  // is corrupted, loading it wholesale can exhaust heap early in boot and
  // brick the main firmware before the web UI is available. In that case,
  // discard it and let the device recover cleanly.
  const size_t maxSafeBytes = 16384;
  const size_t fileSize = f.size();
  if (fileSize > maxSafeBytes) {
    f.close();
    LittleFS.remove(logPath_);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err != DeserializationError::Ok || !doc.is<JsonArray>()) return;

  items_.reserve(maxEntries_);
  bool migratedLegacyEntries = false;
  for (JsonObject item : doc.as<JsonArray>()) {
    EventEntry entry;
    entry.seq = item["seq"] | 0;
    entry.bootId = item["boot_id"] | 0;
    entry.ts = item["ts"] | 0;
    entry.type = String((const char*)(item["type"] | "event"));
    entry.message = String((const char*)(item["message"] | ""));
    if (entry.message.isEmpty()) continue;
    // 0.2.18 sweep / .185 corruption fix: drop entries whose message or
    // type contains non-printable / non-ASCII bytes. The .185 file
    // round-tripped a corrupt String ("Heartbeat transport fail\x25P\x03\xe0")
    // forever because the outer JSON still parsed — but no firmware event
    // string contains anything outside printable ASCII. Treat such an
    // entry as a write-tear or memory-stomp artifact and skip it; the
    // first clean persist after this load rewrites a sanitized file.
    auto isPrintableAscii = [](const String& s) {
      for (size_t i = 0; i < s.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c > 0x7E) return false;
      }
      return true;
    };
    if (!isPrintableAscii(entry.type) || !isPrintableAscii(entry.message)) {
      migratedLegacyEntries = true;  // ensure a clean persist on next flush
      continue;
    }
    if (entry.seq == 0) {
      entry.seq = items_.empty() ? 1 : (items_.back().seq + 1);
      migratedLegacyEntries = true;
    }
    if (entry.bootId == 0) {
      entry.bootId = 1;
      migratedLegacyEntries = true;
    }
    if (items_.size() >= maxEntries_) break;
    items_.push_back(entry);
  }
  if (migratedLegacyEntries) {
    dirty_ = true;
    persist();
  }
}

void EventLog::loop() {
  if (!dirty_) return;

  const uint32_t now = millis();
  if (now < autoPersistAllowedAtMillis_) return;
  if ((now - lastMutationMillis_) < EVENT_LOG_PERSIST_DEBOUNCE_MS) return;
  flush();
}

void EventLog::flush() {
  if (!dirty_) return;

  const uint32_t now = millis();
  if (ESP.getFreeHeap() < EVENT_LOG_MIN_FREE_HEAP) {
    if ((now - lastPersistAttemptMillis_) >= EVENT_LOG_PERSIST_RETRY_MS) {
      lastPersistAttemptMillis_ = now;
    }
    return;
  }

  persist();
  dirty_ = false;
  lastPersistAttemptMillis_ = now;
}

void EventLog::persist() {
  // 0.2.34 BUG-077 fix (c): breadcrumb wraps the whole persist body so
  // a watchdog/exception during JSON serialization, file open, write,
  // remove, or rename surfaces "fs-event-log-write" on the next boot.
  PreCrashBreadcrumb::Scope scope(PreCrashBreadcrumb::OP_FS_EVENT_LOG_WRITE);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& item : items_) {
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = item.seq;
    o["boot_id"] = item.bootId;
    o["ts"] = item.ts;
    o["ts_basis"] = "uptime_seconds";
    o["type"] = item.type;
    o["message"] = item.message;
  }

  // 0.2.18 sweep / .185 corruption fix: atomic temp + rename. Pre-fix the
  // file was opened in-place with "w" (truncate); a reboot mid-serialize
  // (Hardware WDT, power glitch, ESP.restart racing the flash driver) left
  // /events.json half-written. .185 returned a 3583-byte file whose last
  // entry had `"message":"Heartbeat transport fail\x25P\x03\xe0"` — a
  // string truncated mid-write then padded with stale flash bytes. The
  // outer JSON still parsed, so the corrupt entry round-tripped every
  // load+persist cycle indefinitely. Writing to .tmp first keeps the
  // prior known-good file intact until a complete new one exists.
  const String tmpPath = String(logPath_) + ".tmp";
  if (LittleFS.exists(tmpPath)) LittleFS.remove(tmpPath);
  File f = LittleFS.open(tmpPath, "w");
  if (!f) return;
  const size_t written = serializeJson(doc, f);
  f.close();
  if (written == 0) {
    LittleFS.remove(tmpPath);
    return;
  }
  if (LittleFS.exists(logPath_)) LittleFS.remove(logPath_);
  LittleFS.rename(tmpPath, logPath_);
}
