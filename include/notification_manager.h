#pragma once

#include "types.h"

class NotificationManager {
public:
  void begin(AppConfig* config);
  bool send(const String& eventType, const String& reason, const String& detailsJson);
private:
  AppConfig* config_ = nullptr;
};

