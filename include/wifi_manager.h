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
private:
  bool captivePortal_ = false;
  String setupApName_;
};

