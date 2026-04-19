#pragma once

#include <Arduino.h>

#include <functional>

class WifiManagerService {
public:
  bool begin(const String& apName);
  void loop();
  bool isConnected() const;
  String ipAddress() const;
  bool inCaptivePortal() const;
private:
  bool captivePortal_ = false;
};

