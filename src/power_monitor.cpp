#include <cmath>

#include "power_monitor.h"

#include "pins.h"

namespace {
static constexpr uint32_t FRAME_TIMEOUT_MS = 500;
static constexpr size_t CSE7766_FRAME_SIZE = 24;
static constexpr float MIN_MEASURED_CURRENT_A = 0.05f;
// UART-contention detection: if after this long no valid CSE7766 frame has
// been seen but invalid frames are accumulating, GPIO3 is most likely being
// driven by the USB-serial debug header. Surface that to the UI as a hint.
static constexpr uint32_t UART_CONTENTION_GRACE_MS = 20000;
static constexpr uint32_t UART_CONTENTION_MIN_INVALID = 20;
}

void PowerMonitor::begin(RuntimeStatus* status) {
  status_ = status;
  beginAtMs_ = millis();
  pinMode(Pins::POWER_MONITOR_RX, INPUT);
  serial_.begin(4800, SWSERIAL_8E1, Pins::POWER_MONITOR_RX, -1, false, 96, 32);
}

void PowerMonitor::loop() {
  if (!status_) return;

  const uint32_t now = millis();
  if ((now - lastTransmissionAtMs_) >= FRAME_TIMEOUT_MS) {
    rawFrameIndex_ = 0;
  }

  while (serial_.available() > 0) {
    const int nextByte = serial_.read();
    if (nextByte < 0) break;

    lastTransmissionAtMs_ = millis();
    rawFrame_[rawFrameIndex_] = static_cast<uint8_t>(nextByte);
    if (!checkByte_()) {
      status_->power.invalidFrameCount++;
      rawFrameIndex_ = 0;
      continue;
    }

    if (rawFrameIndex_ == (CSE7766_FRAME_SIZE - 1)) {
      parseFrame_();
      rawFrameIndex_ = 0;
      continue;
    }

    rawFrameIndex_++;
  }

  updateUartContention_();
}

void PowerMonitor::updateUartContention_() {
  if (!status_) return;
  // Contended only once past the grace window with no chip ever seen and a
  // meaningful pile of invalid frames. Clears as soon as a real frame parses.
  if (status_->power.chipSeen) {
    status_->power.uartContended = false;
    return;
  }
  const uint32_t elapsed = millis() - beginAtMs_;
  status_->power.uartContended =
      (elapsed >= UART_CONTENTION_GRACE_MS) &&
      (status_->power.invalidFrameCount >= UART_CONTENTION_MIN_INVALID);
}

void PowerMonitor::resetAggregate() {
  if (!status_) return;
  PowerAggregate fresh;
  fresh.windowStartUptimeSeconds = status_->uptimeSeconds;
  fresh.energyWh = status_->power.energyWh;
  status_->power.aggregate = fresh;
}

void PowerMonitor::resetEnergy() {
  cfPulsesTotal_ = 0;
  energyInitialized_ = false;
  if (status_) {
    status_->power.energyWh = 0;
    status_->power.aggregate.energyWh = 0;
  }
}

void PowerMonitor::updateAggregate_() {
  if (!status_) return;
  PowerAggregate& agg = status_->power.aggregate;
  const float w = status_->power.powerW;
  if (!agg.hasData) {
    agg.hasData = true;
    agg.minW = w;
    agg.maxW = w;
    agg.sumW = 0.0f;
    agg.sampleCount = 0;
    if (agg.windowStartUptimeSeconds == 0) {
      agg.windowStartUptimeSeconds = status_->uptimeSeconds;
    }
  }
  if (w < agg.minW) agg.minW = w;
  if (w > agg.maxW) agg.maxW = w;
  agg.sumW += w;
  agg.sampleCount++;
  agg.lastV = status_->power.voltageV;
  agg.lastA = status_->power.currentMa / 1000.0f;
  agg.lastPF = status_->power.powerFactor;
  agg.energyWh = status_->power.energyWh;
}

void PowerMonitor::pushRecentSample_() {
  if (!status_) return;
  PowerRecentSample& slot = recentRing_[recentHead_];
  slot.uptimeSeconds = status_->power.lastSampleUptimeSeconds;
  slot.voltageV = status_->power.voltageV;
  slot.powerW = status_->power.powerW;
  slot.currentMa = status_->power.currentMa;
  slot.powerFactor = status_->power.powerFactor;
  slot.energyWh = status_->power.energyWh;
  slot.sourceFlags = status_->power.sourceFlags;
  recentHead_ = (recentHead_ + 1) % POWER_RECENT_RING_SIZE;
  if (recentCount_ < POWER_RECENT_RING_SIZE) recentCount_++;
}

String PowerMonitor::recentSamplesJson() const {
  String out = "[";
  for (uint8_t i = 0; i < recentCount_; ++i) {
    // Oldest-first ordering.
    const uint8_t idx =
        (recentHead_ + POWER_RECENT_RING_SIZE - recentCount_ + i) % POWER_RECENT_RING_SIZE;
    const PowerRecentSample& s = recentRing_[idx];
    if (i > 0) out += ",";
    out += "{\"uptime_seconds\":" + String(s.uptimeSeconds);
    out += ",\"v_v\":" + String(s.voltageV, 2);
    out += ",\"p_w\":" + String(s.powerW, 2);
    out += ",\"i_ma\":" + String(s.currentMa);
    out += ",\"pf\":" + String(s.powerFactor, 3);
    out += ",\"energy_wh\":" + String(s.energyWh);
    out += ",\"source_flags\":" + String(s.sourceFlags);
    out += "}";
  }
  out += "]";
  return out;
}

bool PowerMonitor::checkByte_() const {
  const uint8_t index = rawFrameIndex_;
  const uint8_t value = rawFrame_[index];

  if (index == 0) {
    return (value == 0x55) || ((value & 0xF0) == 0xF0) || (value == 0xAA);
  }

  if (index == 1) {
    return value == 0x5A;
  }

  if (index == (CSE7766_FRAME_SIZE - 1)) {
    uint8_t checksum = 0;
    for (uint8_t i = 2; i < (CSE7766_FRAME_SIZE - 1); ++i) {
      checksum += rawFrame_[i];
    }
    return checksum == rawFrame_[CSE7766_FRAME_SIZE - 1];
  }

  return true;
}

uint32_t PowerMonitor::read24Bit_(uint8_t startIndex) const {
  return (static_cast<uint32_t>(rawFrame_[startIndex]) << 16) |
         (static_cast<uint32_t>(rawFrame_[startIndex + 1]) << 8) |
         static_cast<uint32_t>(rawFrame_[startIndex + 2]);
}

void PowerMonitor::parseFrame_() {
  if (!status_) return;

  const uint8_t header1 = rawFrame_[0];
  if (header1 == 0xAA) {
    status_->power.invalidFrameCount++;
    return;
  }

  bool powerCycleExceedsRange = false;
  if ((header1 & 0xF0) == 0xF0) {
    if (header1 & 0x0D) {
      status_->power.invalidFrameCount++;
      return;
    }
    powerCycleExceedsRange = (header1 & (1 << 1)) != 0;
  }

  const uint32_t voltageCoeff = read24Bit_(2);
  const uint32_t voltageCycle = read24Bit_(5);
  const uint32_t currentCoeff = read24Bit_(8);
  const uint32_t currentCycle = read24Bit_(11);
  const uint32_t powerCoeff = read24Bit_(14);
  const uint32_t powerCycle = read24Bit_(17);
  const uint8_t adj = rawFrame_[20];
  const uint16_t cfPulses =
      (static_cast<uint16_t>(rawFrame_[21]) << 8) | rawFrame_[22];

  bool voltageValid = (adj & 0x40) != 0 && voltageCoeff > 0 && voltageCycle > 0;
  bool currentValid = (adj & 0x20) != 0 && currentCoeff > 0 && currentCycle > 0;
  bool powerValid = false;

  float voltageV = 0.0f;
  if (voltageValid) {
    voltageV = voltageCoeff / static_cast<float>(voltageCycle);
  }

  float powerW = 0.0f;
  if (powerCycleExceedsRange) {
    powerValid = true;
    powerW = 0.0f;
  } else if ((adj & 0x10) != 0 && powerCoeff > 0 && powerCycle > 0) {
    powerValid = true;
    powerW = powerCoeff / static_cast<float>(powerCycle);
  }

  float currentA = 0.0f;
  float estimatedCurrentA = 0.0f;
  bool currentEstimated = false;
  if (currentValid) {
    if (powerValid && voltageV > 1.0f) {
      estimatedCurrentA = powerW / voltageV;
    }
    if (estimatedCurrentA > MIN_MEASURED_CURRENT_A) {
      currentA = currentCoeff / static_cast<float>(currentCycle);
    } else {
      currentEstimated = estimatedCurrentA > 0.0f;
      currentValid = false;
    }
  } else if (powerValid && voltageV > 1.0f) {
    estimatedCurrentA = powerW / voltageV;
    currentEstimated = estimatedCurrentA > 0.0f;
  }

  const float apparentPowerVa = voltageValid ? (voltageV * currentA) : 0.0f;

  float powerFactor = 1.0f;
  if (apparentPowerVa > 0.0f && powerValid) {
    powerFactor = powerW / apparentPowerVa;
    if (powerFactor < 0.0f || powerFactor > 1.0f) {
      powerFactor = 1.0f;
    }
  } else if (powerW > 0.0f && apparentPowerVa <= 0.0f) {
    powerFactor = 0.0f;
  }

  bool energyValid = powerCoeff > 0;
  uint32_t energyWh = status_->power.energyWh;
  if (energyValid) {
    if (!energyInitialized_) {
      cfPulsesLast_ = cfPulses;
      energyInitialized_ = true;
    }
    const uint16_t pulseDelta = static_cast<uint16_t>(cfPulses - cfPulsesLast_);
    cfPulsesTotal_ += pulseDelta;
    cfPulsesLast_ = cfPulses;

    const float energyKwh =
        (cfPulsesTotal_ * static_cast<float>(powerCoeff)) / 1000000.0f / 3600.0f;
    energyWh = static_cast<uint32_t>(lroundf(energyKwh * 1000.0f));
  }

  if (!voltageValid && !powerValid && !currentValid && !energyValid) {
    status_->power.invalidFrameCount++;
    return;
  }

  status_->power.chipSeen = true;
  status_->power.realSample = true;
  status_->power.voltageValid = voltageValid;
  status_->power.currentValid = currentValid;
  status_->power.powerValid = powerValid;
  status_->power.frequencyValid = false;
  status_->power.energyValid = energyValid;
  status_->power.currentEstimated = currentEstimated;
  status_->power.sourceFlags = POWER_SAMPLE_FLAG_REAL |
      (voltageValid ? POWER_SAMPLE_FLAG_VOLTAGE_VALID : 0) |
      (currentValid ? POWER_SAMPLE_FLAG_CURRENT_VALID : 0) |
      (powerValid ? POWER_SAMPLE_FLAG_POWER_VALID : 0) |
      (energyValid ? POWER_SAMPLE_FLAG_ENERGY_VALID : 0) |
      (currentEstimated ? POWER_SAMPLE_FLAG_CURRENT_ESTIMATED : 0);
  status_->power.lastSampleMillis = millis();
  status_->power.lastSampleUptimeSeconds = status_->power.lastSampleMillis / 1000UL;
  status_->power.validFrameCount++;
  status_->power.voltageV = voltageV;
  status_->power.currentMa = static_cast<uint32_t>(lroundf(currentA * 1000.0f));
  status_->power.estimatedCurrentMa = static_cast<uint32_t>(lroundf(estimatedCurrentA * 1000.0f));
  status_->power.powerW = powerW;
  status_->power.apparentPowerVa = apparentPowerVa;
  status_->power.powerFactor = powerFactor;
  status_->power.frequencyHz = 0.0f;
  status_->power.energyWh = energyWh;
  status_->power.uartContended = false;

  updateAggregate_();
  pushRecentSample_();
}
