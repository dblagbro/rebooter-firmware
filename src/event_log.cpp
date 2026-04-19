#include <Arduino.h>
#include <ArduinoJson.h>
#include "event_log.h"

void EventLog::begin(uint16_t maxEntries) {
  maxEntries_ = maxEntries;
}

void EventLog::add(const String& type, const String& message) {
  if (items_.size() >= maxEntries_) items_.erase(items_.begin());
  items_.push_back({millis() / 1000UL, type, message});
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

