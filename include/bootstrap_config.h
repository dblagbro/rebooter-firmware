#pragma once

namespace BootstrapConfig {
  // Primary network
  static constexpr char WIFI_SSID[] = "VoIPguru_wifi";
  static constexpr char WIFI_PASSWORD[] = "whowantstoknow";

  // Secondary network (GF's house)
  static constexpr char WIFI_SSID2[] = "SpectrumSetup-4D";
  static constexpr char WIFI_PASSWORD2[] = "smalltruck536";

  // Fall back to any open (unsecured) network if both configured SSIDs fail
  static constexpr bool OPEN_WIFI_FALLBACK = true;
  // Minimum RSSI to consider an open network usable
  static constexpr int8_t OPEN_WIFI_MIN_RSSI = -80;

  static constexpr char FIRMWARE_BASE_URL[] = "https://www.voipguru.org/rebooter/firmware/stable/";
  static constexpr char FIRMWARE_BASE_URL2[] = "https://www2.voipguru.org/rebooter/firmware/stable/";
  static constexpr char FIRMWARE_FILENAME[] = "latest.bin";
  static constexpr char CURRENT_VERSION[] = "bootstrap-0.2.4";

  static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
  static constexpr uint32_t WIFI_RETRY_DELAY_MS = 10000;
  static constexpr uint32_t UPDATE_RETRY_DELAY_MS = 30000;
  static constexpr uint8_t MAX_UPDATE_ATTEMPTS = 5;
}
