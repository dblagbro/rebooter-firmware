#pragma once

#include <Arduino.h>
#include <vector>

struct EventEntry {
  uint32_t seq;
  uint32_t bootId;
  uint32_t ts;
  String type;
  String message;
};

class EventLog {
public:
  void begin(uint16_t maxEntries);
  void add(const String& type, const String& message);
  String asJson() const;
  void loop();
  void flush();
private:
  void trimToLimit();
  void load();
  void persist();

  const char* logPath_ = "/events.json";
  uint16_t maxEntries_ = 30;  // 0.2.6: hard-capped to 30 in begin() — see note there
  uint32_t suppressDuplicateWindowSeconds_ = 120;
  uint32_t nextSeq_ = 1;
  uint32_t bootId_ = 1;
  std::vector<EventEntry> items_;
  bool dirty_ = false;
  uint32_t lastMutationMillis_ = 0;
  uint32_t lastPersistAttemptMillis_ = 0;
  uint32_t autoPersistAllowedAtMillis_ = 0;
};
