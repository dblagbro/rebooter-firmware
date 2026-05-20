#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "event_log.h"

namespace {
constexpr uint32_t EVENT_LOG_PERSIST_DEBOUNCE_MS = 1500UL;
constexpr uint32_t EVENT_LOG_PERSIST_RETRY_MS = 5000UL;
constexpr uint32_t EVENT_LOG_MIN_FREE_HEAP = 12000UL;
constexpr uint32_t EVENT_LOG_BOOT_QUIET_PERIOD_MS = 600000UL;
}

void EventLog::begin(uint16_t maxEntries) {
  maxEntries_ = max<uint16_t>(25, min<uint16_t>(maxEntries, 100));
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
  String cleanType = type;
  cleanType.trim();
  if (cleanType.isEmpty()) cleanType = "event";
  if (cleanType.length() > 32) cleanType = cleanType.substring(0, 32);

  String cleanMessage = message;
  cleanMessage.trim();
  if (cleanMessage.length() > 160) {
    cleanMessage = cleanMessage.substring(0, 157) + "...";
  }

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
  if (!LittleFS.exists(logPath_)) return;

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

  File f = LittleFS.open(logPath_, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}
