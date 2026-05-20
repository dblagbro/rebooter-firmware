#pragma once

#include <ArduinoJson.h>

#include "types.h"
#include "app_state.h"

namespace StatusPayload {

String modeToString(DeviceMode mode);
String healthToString(HealthState state);
String restoreToString(RelayRestoreBehavior value);

void fillReportedConfig(JsonObject target, const AppConfig& config);
void fillPowerStatus(JsonDocument& doc, const AppConfig& config,
                     const RuntimeStatus* status);
void fillHeartbeatDocument(JsonDocument& doc, const AppConfig& config,
                           const RuntimeStatus* status,
                           const String& firmwareVersion,
                           bool includeReportedConfig,
                           bool compactMode = false);

}
