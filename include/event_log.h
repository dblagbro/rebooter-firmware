#pragma once

#include <Arduino.h>
#include <vector>

struct EventEntry {
  uint32_t ts;
  String type;
  String message;
};

class EventLog {
public:
  void begin(uint16_t maxEntries);
  void add(const String& type, const String& message);
  String asJson() const;
private:
  void trimToLimit();
  void load();
  void persist() const;

  const char* logPath_ = "/events.json";
  uint16_t maxEntries_ = 200;
  std::vector<EventEntry> items_;
};