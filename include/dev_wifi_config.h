#pragma once

namespace DevWifiConfig {
  static constexpr bool ENABLED = true;

  // Primary network
  static constexpr char SSID[] = "VoIPguru_wifi";
  static constexpr char PASSWORD[] = "whowantstoknow";

  // Secondary network
  static constexpr char SSID2[] = "SpectrumSetup-4D";
  static constexpr char PASSWORD2[] = "smalltruck536";

  // Fall back to any open (unsecured) network if both configured SSIDs fail
  static constexpr bool OPEN_WIFI_FALLBACK = true;
  static constexpr int8_t OPEN_WIFI_MIN_RSSI = -80;

  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

  // Time with no WiFi before entering AP mode (2 minutes)
  static constexpr uint32_t AP_FALLBACK_TIMEOUT_MS = 120000;
}
