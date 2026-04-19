#include <Arduino.h>
#include "pins.h"
#include "relay_controller.h"

void RelayController::begin(bool initialOn) {
  pinMode(Pins::RELAY, OUTPUT);
  set(initialOn);
}

void RelayController::set(bool on) {
  relayOn_ = on;
  digitalWrite(Pins::RELAY, on ? HIGH : LOW);
}

void RelayController::toggle() {
  set(!relayOn_);
}

bool RelayController::isOn() const {
  return relayOn_;
}