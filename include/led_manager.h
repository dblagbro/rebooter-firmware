#pragma once

#include <Arduino.h>

enum class LedPattern : uint8_t {
  Off,
  Solid,
  SlowBlink,
  FastBlink,
  DoubleBlink
};

class LedManager {
public:
  void begin();
  void setPattern(LedPattern pattern);
  void loop();
private:
  LedPattern pattern_ = LedPattern::Off;
  uint32_t lastTick_ = 0;
  bool state_ = false;
};

