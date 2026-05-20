#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

#include "types.h"
#include "app_state.h"

// LAN discovery beacon.
//
// Two cooperating mechanisms:
//   - mDNS: advertises hostname rebooter-<chipid6>.local and an
//     _rebooter._tcp service on port 80 with TXT records. OPT-IN / off by
//     default because the responder carries a ~1-2 KB standing heap cost
//     that can erode the low-heap floor.
//   - UDP broadcast burst: a short, time-bounded "I just joined" announce
//     on a fixed port. Near-free, always-on, covers the AP-setup handoff
//     window where the app is actively looking for the device.
class DiscoveryManager {
public:
  void begin(AppConfig* config, RuntimeStatus* status);
  void loop();

  // Called when Wi-Fi transitions to connected (starts mDNS if enabled and
  // arms the boot UDP burst).
  void onWifiConnected();

  // Called once after a fresh captive-portal provisioning to arm the longer
  // post-setup UDP announce window.
  void onPortalProvisioned();

  // Triggers an on-demand UDP announce burst (POST /api/discovery/announce).
  void triggerAnnounceBurst();

private:
  void startMdns_();
  void sendAnnouncePacket_();
  String buildAnnouncePayload_() const;
  String hostname_() const;

  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
  WiFiUDP udp_;

  bool mdnsActive_ = false;
  bool wasConnected_ = false;

  // Announce window state. announceUntilMs_ == 0 means no window active.
  uint32_t announceUntilMs_ = 0;
  uint32_t nextAnnounceAtMs_ = 0;
  uint32_t announceIntervalMs_ = 5000;
};
