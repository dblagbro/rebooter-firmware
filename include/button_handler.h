#pragma once

#include <Arduino.h>

class ButtonHandler {
public:
  void begin();
  void loop();
  bool shortPressed();
  bool longPressed3s();
  bool longPressed10s();
  bool longPressed30s();
private:
  bool shortPressed_ = false;
  bool longPressed3s_ = false;
  bool longPressed10s_ = false;
  bool longPressed30s_ = false;
  bool wasDown_ = false;
  uint32_t downStart_ = 0;
};

