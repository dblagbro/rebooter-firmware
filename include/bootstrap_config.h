#pragma once

namespace BootstrapConfig {
  static constexpr char WIFI_SSID[] = "SpectrumSetup-4D";
  static constexpr char WIFI_PASSWORD[] = "smalltruck536";

  static constexpr char FIRMWARE_BASE_URL[] = "https://www.voipguru.org/";
  static constexpr char FIRMWARE_FILENAME[] = "rebooter-firmware-main.bin";
  static constexpr char CURRENT_VERSION[] = "bootstrap-0.1.0";

  static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
  static constexpr uint32_t WIFI_RETRY_DELAY_MS = 10000;
  static constexpr uint32_t UPDATE_RETRY_DELAY_MS = 30000;
  static constexpr uint8_t MAX_UPDATE_ATTEMPTS = 5;
}
