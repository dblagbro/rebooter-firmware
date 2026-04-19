#pragma once

#include <Arduino.h>
#include <ESP8266WebServer.h>

class EventLog;

class OtaManager {
public:
  void begin(EventLog* eventLog);
  void handleUpload(HTTPUpload& upload);
  bool hasError() const;
  String errorString() const;
private:
  EventLog* eventLog_ = nullptr;
  bool updateStarted_ = false;
  bool updateError_ = false;
  uint8_t lastError_ = 0;
};