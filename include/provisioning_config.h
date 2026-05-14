#pragma once

#include <Arduino.h>

namespace ProvisioningConfig {
  static constexpr char SETUP_AP_PREFIX[] = "Rebooter-Setup";
  static constexpr char SETUP_AP_PASSWORD[] = "";
  static constexpr uint32_t CONFIG_PORTAL_TIMEOUT_SECONDS = 0;

  inline String setupApName(uint32_t chipId) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s-%06X", SETUP_AP_PREFIX, static_cast<unsigned int>(chipId & 0xFFFFFF));
    return String(buffer);
  }
}
