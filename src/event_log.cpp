#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "event_log.h"

void EventLog::begin(uint16_t maxEntries) {
  maxEntries_ = maxEntries;
  load();
  trimToLimit();
}

void EventLog::add(const String& type, const String& message) {
  items_.push_back({millis() / 1000UL, type, message});
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

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err != DeserializationError::Ok || !doc.is<JsonArray>()) return;

  for (JsonObject item : doc.as<JsonArray>()) {
    EventEntry entry;
    entry.ts = item["ts"] | 0;
    entry.type = String((const char*)(item["type"] | "event"));
    entry.message = String((const char*)(item["message"] | ""));
    if (!entry.message.isEmpty()) items_.push_back(entry);
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