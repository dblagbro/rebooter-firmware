#include <Arduino.h>
#include <LittleFS.h>

#include "pins.h"
#include "types.h"
#include "app_state.h"
#include "config_manager.h"
#include "relay_controller.h"
#include "led_manager.h"
#include "button_handler.h"
#include "wifi_manager.h"
#include "monitor_engine.h"
#include "notification_manager.h"
#include "web_server_manager.h"
#include "event_log.h"
#include "ota_manager.h"
#include "auth_manager.h"

AppConfig g_config;
RuntimeStatus g_status;
ConfigManager g_cfgMgr;
RelayController g_relay;
LedManager g_led;
ButtonHandler g_button;
WifiManagerService g_wifi;
NotificationManager g_notifier;
EventLog g_eventLog;
MonitorEngine g_monitor;
WebServerManager g_web;
OtaManager g_ota;
AuthManager g_auth;

static bool initialRelayStateFromConfig() {
  switch (g_config.relayRestoreBehavior) {
    case RelayRestoreBehavior::AlwaysOn: return true;
    case RelayRestoreBehavior::AlwaysOff: return false;
    default: return g_config.lastRelayOn;
  }
}

static void persistManualRelayState() {
  g_config.lastRelayOn = g_relay.isOn();
  g_cfgMgr.save(g_config);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  LittleFS.begin();
  g_cfgMgr.begin();
  g_cfgMgr.load(g_config);

  g_eventLog.begin(g_config.eventLogMaxEntries);
  g_eventLog.add("boot", "Device booting");

  g_relay.begin(initialRelayStateFromConfig());
  g_status.relayOn = g_relay.isOn();
  g_led.begin();
  g_button.begin();

  g_wifi.begin(g_config.deviceName);
  g_notifier.begin(&g_config);
  g_monitor.begin(&g_config, &g_status, &g_relay, &g_notifier, &g_eventLog);
  g_ota.begin(&g_eventLog);
  g_auth.begin(&g_config, &g_eventLog);
  g_web.begin(&g_config, &g_status, &g_relay, &g_cfgMgr, &g_eventLog, &g_monitor, &g_ota, &g_auth);

  g_led.setPattern(LedPattern::SlowBlink);
}

void loop() {
  g_wifi.loop();
  g_button.loop();
  g_led.loop();
  g_web.loop();
  g_monitor.loop();

  g_status.uptimeSeconds = millis() / 1000;
  g_status.wifiConnected = g_wifi.isConnected();
  g_status.inCaptivePortal = g_wifi.inCaptivePortal();
  g_status.relayOn = g_relay.isOn();

  if (g_button.shortPressed() && g_config.currentMode == DeviceMode::SmartPlug && g_config.manualButtonEnabled) {
    g_relay.toggle();
    persistManualRelayState();
    g_eventLog.add("relay", g_relay.isOn() ? "Relay turned on by button" : "Relay turned off by button");
  }

  if (g_button.longPressed5s()) {
    g_eventLog.add("system", "Reboot requested by button");
    ESP.restart();
  }

  if (g_button.longPressed10s()) {
    g_eventLog.add("system", "Factory reset requested by button");
    g_cfgMgr.reset();
    ESP.restart();
  }
}