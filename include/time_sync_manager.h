#pragma once

#include "app_state.h"

class TimeSyncManager {
public:
  void begin(RuntimeStatus* status);
  void loop(bool wifiConnected);

private:
  void refreshClockStatus_(uint32_t nowMs);

  RuntimeStatus* status_ = nullptr;
  bool configured_ = false;
  uint32_t lastConfigAttemptAtMs_ = 0;
};
