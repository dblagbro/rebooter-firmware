#pragma once

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include "types.h"

class EventLog;

class AuthManager {
public:
  void begin(AppConfig* config, EventLog* eventLog);
  bool isProvisioned() const;
  bool setPassword(const String& username, const String& password);
  bool isAuthorized(ESP8266WebServer& server);
  bool requireAuth(ESP8266WebServer& server);
private:
  String hashPassword(const String& password) const;
  void reject(ESP8266WebServer& server);

  AppConfig* config_ = nullptr;
  EventLog* eventLog_ = nullptr;
};