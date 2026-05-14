#pragma once

namespace DevWifiConfig {
  struct Network {
    const char* ssid;
    const char* password;
  };

  static constexpr bool ENABLED = true;
  static constexpr char PRIMARY_SSID[] = "SpectrumSetup-4D";
  static constexpr char PRIMARY_PASSWORD[] = "smalltruck536";
  static constexpr char SECONDARY_SSID[] = "VoIPguru_wifi";
  static constexpr char SECONDARY_PASSWORD[] = "whowantstoknow";
  static constexpr Network NETWORKS[] = {
    {PRIMARY_SSID, PRIMARY_PASSWORD},
    {SECONDARY_SSID, SECONDARY_PASSWORD},
  };
  static constexpr size_t NETWORK_COUNT = sizeof(NETWORKS) / sizeof(NETWORKS[0]);
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
}
