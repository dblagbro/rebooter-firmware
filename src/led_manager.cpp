#include <Arduino.h>
#include "pins.h"
#include "led_manager.h"

void LedManager::begin() {
  pinMode(Pins::LED, OUTPUT);
  digitalWrite(Pins::LED, HIGH);
}

void LedManager::setPattern(LedPattern pattern) {
  pattern_ = pattern;
}

void LedManager::loop() {
  const uint32_t now = millis();
  uint32_t interval = 0;

  switch (pattern_) {
    case LedPattern::Off:
      digitalWrite(Pins::LED, HIGH);
      return;
    case LedPattern::Solid:
      digitalWrite(Pins::LED, LOW);
      return;
    case LedPattern::SlowBlink:
      interval = 800; break;
    case LedPattern::FastBlink:
      interval = 150; break;
    case LedPattern::DoubleBlink:
      interval = 100; break;
  }

  if (now - lastTick_ >= interval) {
    lastTick_ = now;
    state_ = !state_;
    digitalWrite(Pins::LED, state_ ? LOW : HIGH);
  }
}

