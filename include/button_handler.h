#pragma once

#include <Arduino.h>

class ButtonHandler {
public:
  void begin();
  void loop();
  bool shortPressed();
  bool longPressed5s();
  bool longPressed10s();
private:
  bool shortPressed_ = false;
  bool longPressed5s_ = false;
  bool longPressed10s_ = false;
  bool wasDown_ = false;
  uint32_t downStart_ = 0;
};

