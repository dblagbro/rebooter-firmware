#pragma once

#include <Arduino.h>

#include <vector>

#include "types.h"

class WifiManagerService {
public:
  // Connection lifecycle state.
  enum class State : uint8_t {
    Init = 0,
    Connecting,
    Connected,
    Portal,
    Reconnecting
  };

  // begin() builds the boot-time candidate list (saved networks -> built-in
  // dev networks -> AP portal) and walks it. config may be null, in which
  // case only the built-in dev networks and the portal are used. If the AP
  // portal runs and the operator provisions Wi-Fi, the provisioned SSID and
  // any hub URLs entered on the portal page are written back into config so
  // they persist as the device's own saved list.
  bool begin(const String& apName, AppConfig* config, bool forcePortal = false);
  void loop();
  bool isConnected() const;
  String ipAddress() const;
  bool inCaptivePortal() const;
  String setupApName() const;
  bool provisionedViaPortal() const;
  // True when begin() captured operator-entered portal fields into AppConfig;
  // the caller should persist the config when this is set.
  bool configChangedByPortal() const;
  void clearProvisionedCredentials();

  // Scan for nearby access points. Returns a JSON array string of
  // {"ssid","rssi","secure"} objects. Synchronous; frees the scan result
  // immediately. Briefly disrupts the link, so gate it behind an explicit UI
  // action.
  String scanNetworksJson();

  // 0.2.8 (#154): opt-in periodic *async* nearby-network scan. Drives a
  // non-blocking scan state machine (no main-loop stall / WDT risk) gated on
  // config flag + a heap floor; stashes a compact top-N summary the heartbeat
  // can carry. Call every loop. `freeHeap` is passed in so the manager
  // doesn't kick off a scan when memory is tight (the scan itself allocates).
  void loopPeriodicScan(const AppConfig* config, uint32_t freeHeap);
  // Compact JSON array of the most recent periodic scan's top networks
  // (`[{"ssid","rssi"}]`), or "" if none captured yet / stale. Cheap copy.
  const String& latestScanSummary() const { return periodicScanSummary_; }
  uint32_t latestScanUptimeSeconds() const { return periodicScanUptimeSeconds_; }

  // 0.2.43 (BUG-087 follow-up): boot walk decision trace. Serial.print
  // is invisible on prod (Sonoff S31 = no accessible UART), so capture
  // walk decisions into a small buffer that main.cpp emits over
  // DiagSyslog AFTER WiFi comes up. Empty until begin() runs.
  const String& walkTrace() const { return walkTrace_; }
  uint16_t walkAttempts() const { return walkAttempts_; }
  bool walkFellToPortal() const { return walkFellToPortal_; }

private:
  struct Candidate {
    String ssid;
    String password;
    bool fromSaved = false;   // true = user saved network, false = dev fallback
  };

  void buildCandidateList(const AppConfig* config);
  bool attemptCandidate(const Candidate& candidate);
  bool walkCandidates();
  bool startPortal(const String& apName, bool forcePortal, AppConfig* config);

  State state_ = State::Init;
  bool captivePortal_ = false;
  bool provisionedViaPortal_ = false;
  bool configChangedByPortal_ = false;
  String setupApName_;
  String apName_;

  std::vector<Candidate> candidates_;
  String walkTrace_;                          // 0.2.43 walk decision log
  uint16_t walkAttempts_ = 0;                 // 0.2.43 count of attemptCandidate calls
  bool walkFellToPortal_ = false;             // 0.2.43 true if begin() went to portal
  uint32_t connectTimeoutMs_ = 15000;
  uint32_t lastLinkOkMs_ = 0;
  uint32_t nextReconnectAttemptMs_ = 0;

  // 0.2.8 (#154) periodic async-scan state.
  bool periodicScanInFlight_ = false;
  uint32_t periodicScanStartedMs_ = 0;       // for the async-scan timeout guard
  uint32_t periodicScanLastRunMs_ = 0;       // throttles to the configured interval
  uint32_t periodicScanUptimeSeconds_ = 0;   // when the last summary was captured
  String periodicScanSummary_;               // compact "[{ssid,rssi}]" top-N
};
