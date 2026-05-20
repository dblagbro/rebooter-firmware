#pragma once

#include <Arduino.h>

#include <functional>

class WifiManagerService {
public:
  bool begin(const String& apName, bool forcePortal = false);
  void loop();
  bool isConnected() const;
  String ipAddress() const;
  bool inCaptivePortal() const;
  String setupApName() const;
  bool provisionedViaPortal() const;
  void clearProvisionedCredentials();
private:
  bool captivePortal_ = false;
  bool provisionedViaPortal_ = false;
  String setupApName_;
};

