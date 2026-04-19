#pragma once

#include "types.h"

class ConfigManager {
public:
  bool begin();
  bool load(AppConfig& out);
  bool save(const AppConfig& config);
  bool reset();
private:
  const char* configPath_ = "/config.json";
};

