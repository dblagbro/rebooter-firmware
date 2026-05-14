#pragma once

namespace BootstrapConfig {
  struct Network {
    const char* ssid;
    const char* password;
  };

  static constexpr char PRIMARY_WIFI_SSID[] = "SpectrumSetup-4D";
  static constexpr char PRIMARY_WIFI_PASSWORD[] = "smalltruck536";
  static constexpr char SECONDARY_WIFI_SSID[] = "VoIPguru_wifi";
  static constexpr char SECONDARY_WIFI_PASSWORD[] = "whowantstoknow";
  static constexpr Network WIFI_NETWORKS[] = {
    {PRIMARY_WIFI_SSID, PRIMARY_WIFI_PASSWORD},
    {SECONDARY_WIFI_SSID, SECONDARY_WIFI_PASSWORD},
  };
  static constexpr size_t WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

  static constexpr char PRIMARY_FIRMWARE_URL[] = "https://www.voipguru.org/rebooter/firmware/stable/latest.bin";
  static constexpr char SECONDARY_FIRMWARE_URL[] = "https://www2.voipguru.org/rebooter/firmware/stable/latest.bin";
  static constexpr const char* FIRMWARE_URLS[] = {
    PRIMARY_FIRMWARE_URL,
    SECONDARY_FIRMWARE_URL
  };
  static constexpr size_t FIRMWARE_URL_COUNT = sizeof(FIRMWARE_URLS) / sizeof(FIRMWARE_URLS[0]);
  static constexpr char CURRENT_VERSION[] = "bootstrap-0.2.5-dev-safe";

  static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
  static constexpr uint32_t WIFI_RETRY_DELAY_MS = 10000;
  static constexpr uint32_t UPDATE_RETRY_DELAY_MS = 30000;
  static constexpr uint8_t MAX_UPDATE_ATTEMPTS = 5;
}
