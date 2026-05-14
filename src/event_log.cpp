#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "event_log.h"

void EventLog::begin(uint16_t maxEntries) {
  maxEntries_ = max<uint16_t>(25, min<uint16_t>(maxEntries, 100));
  load();
  trimToLimit();
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
        nowSeconds >= last.ts &&
        (nowSeconds - last.ts) < suppressDuplicateWindowSeconds_) {
      return;
    }
  }

  items_.push_back({nowSeconds, cleanType, cleanMessage});
  trimToLimit();
  persist();
}

String EventLog::asJson() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& item : items_) {
    JsonObject o = arr.add<JsonObject>();
    o["ts"] = item.ts;
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
  for (JsonObject item : doc.as<JsonArray>()) {
    EventEntry entry;
    entry.ts = item["ts"] | 0;
    entry.type = String((const char*)(item["type"] | "event"));
    entry.message = String((const char*)(item["message"] | ""));
    if (entry.message.isEmpty()) continue;
    if (items_.size() >= maxEntries_) break;
    items_.push_back(entry);
  }
}

void EventLog::persist() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& item : items_) {
    JsonObject o = arr.add<JsonObject>();
    o["ts"] = item.ts;
    o["type"] = item.type;
    o["message"] = item.message;
  }

  File f = LittleFS.open(logPath_, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}
