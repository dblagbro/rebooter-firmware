#include <Arduino.h>
#include "pins.h"
#include "button_handler.h"

void ButtonHandler::begin() {
  pinMode(Pins::BUTTON, INPUT_PULLUP);
}

void ButtonHandler::loop() {
  shortPressed_ = false;
  longPressed3s_ = false;
  longPressed10s_ = false;
  longPressed30s_ = false;

  bool down = digitalRead(Pins::BUTTON) == LOW;
  const uint32_t now = millis();

  if (down && !wasDown_) {
    wasDown_ = true;
    downStart_ = now;
  } else if (!down && wasDown_) {
    uint32_t held = now - downStart_;
    wasDown_ = false;
    if (held >= 30000) longPressed30s_ = true;
    else if (held >= 10000) longPressed10s_ = true;
    else if (held >= 3000) longPressed3s_ = true;
    else if (held >= 50) shortPressed_ = true;
  }
}

bool ButtonHandler::shortPressed() { return shortPressed_; }
bool ButtonHandler::longPressed3s() { return longPressed3s_; }
bool ButtonHandler::longPressed10s() { return longPressed10s_; }
bool ButtonHandler::longPressed30s() { return longPressed30s_; }

