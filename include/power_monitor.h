#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>

#include "app_state.h"

class PowerMonitor {
public:
  void begin(RuntimeStatus* status);
  void loop();

private:
  bool checkByte_() const;
  void parseFrame_();
  uint32_t read24Bit_(uint8_t startIndex) const;

  RuntimeStatus* status_ = nullptr;
  SoftwareSerial serial_;
  uint8_t rawFrame_[24] = {0};
  uint8_t rawFrameIndex_ = 0;
  uint16_t cfPulsesLast_ = 0;
  uint32_t cfPulsesTotal_ = 0;
  uint32_t lastTransmissionAtMs_ = 0;
  bool energyInitialized_ = false;
};
