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

void setup() {
  Serial.begin(115200);
  delay(200);

  LittleFS.begin();
  g_cfgMgr.begin();
  g_cfgMgr.load(g_config);

  g_eventLog.begin(g_config.eventLogMaxEntries);
  g_eventLog.add("boot", "Device booting");

  g_relay.begin();
  g_led.begin();
  g_button.begin();

  g_wifi.begin(g_config.deviceName);
  g_notifier.begin(&g_config);
  g_monitor.begin(&g_config, &g_status, &g_relay, &g_notifier, &g_eventLog);
  g_web.begin(&g_config, &g_status, &g_relay, &g_cfgMgr, &g_eventLog, &g_monitor);

  g_led.setPattern(LedPattern::SlowBlink);
}

void loop() {
  g_wifi.loop();
  g_button.loop();
  g_led.loop();
  g_web.loop();
  g_monitor.loop();

  g_status.uptimeSeconds = millis() / 1000;

  if (g_button.shortPressed() && g_config.currentMode == DeviceMode::SmartPlug && g_config.manualButtonEnabled) {
    g_relay.toggle();
    g_eventLog.add("relay", g_relay.isOn() ? "Relay turned on by button" : "Relay turned off by button");
  }

  if (g_button.longPressed10s()) {
    g_eventLog.add("system", "Factory reset requested by button");
    g_cfgMgr.reset();
    ESP.restart();
  }
}

