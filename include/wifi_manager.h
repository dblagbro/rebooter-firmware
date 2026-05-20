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
  uint32_t connectTimeoutMs_ = 15000;
  uint32_t lastLinkOkMs_ = 0;
  uint32_t nextReconnectAttemptMs_ = 0;
};
