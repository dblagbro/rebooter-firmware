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
  uint16_t maxEntries_ = 200;
  std::vector<EventEntry> items_;
};

