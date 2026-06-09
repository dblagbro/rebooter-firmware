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
#include "central_client.h"
#include "power_monitor.h"
#include "time_sync_manager.h"
#include "crash_recorder.h"
#include "pre_crash_breadcrumb.h"
#include "udp_control.h"
#include "diag_syslog.h"
#include "discovery_manager.h"
#include "firmware_version.h"

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
CentralClient g_central;
PowerMonitor g_power;
TimeSyncManager g_timeSync;
DiscoveryManager g_discovery;
static bool g_bootMarkedHealthy = false;
static bool g_powerStarted = false;
#ifdef SAFE_FALLBACK_TEST_BAD_BOOT
static bool g_badBootTestTriggered = false;

#ifndef SAFE_FALLBACK_TEST_BAD_BOOT_DELAY_SECONDS
#define SAFE_FALLBACK_TEST_BAD_BOOT_DELAY_SECONDS 15
#endif
#endif

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

static void enterRecoveryMode() {
  g_eventLog.add("system", "Recovery mode requested by button");
  g_eventLog.flush();
  g_cfgMgr.prepareForPlannedRestart("button_recovery_boot");
  g_cfgMgr.requestRecoveryBoot();
  ESP.restart();
}

static void prepareForPlannedRestart(const String& reason) {
  g_cfgMgr.prepareForPlannedRestart(reason);
}

static void maybeTriggerBadBootTest() {
#ifdef SAFE_FALLBACK_TEST_BAD_BOOT
  if (g_status.recoveryMode || g_bootMarkedHealthy || g_badBootTestTriggered) return;
  if (g_status.uptimeSeconds < SAFE_FALLBACK_TEST_BAD_BOOT_DELAY_SECONDS) return;

  g_badBootTestTriggered = true;
  g_eventLog.add("boot", "Intentional bad-firmware test crash firing before healthy mark");
  delay(150);
  abort();
#endif
}

void setup() {
  Serial.begin(115200);
  delay(200);

  g_status = RuntimeStatus();
  g_bootMarkedHealthy = false;
  g_powerStarted = false;
#ifdef SAFE_FALLBACK_TEST_BAD_BOOT
  g_badBootTestTriggered = false;
#endif
  g_status.resetReason = ESP.getResetReason();
  g_status.bootHealthyMarked = false;

  LittleFS.begin();
  // Convert any RTC crash record left by custom_crash_callback into a
  // LittleFS crash file. Done in normal-boot context where allocation is safe.
  const bool crashCaptured = CrashRecorder::processPendingCrash();
  // 0.2.22 (#183): if the previous boot ended mid-operation (Hardware
  // WDT, SDK system_restart, anything that bypasses the user-level
  // crash_callback), this surfaces which operation was in flight.
  PreCrashBreadcrumb::processPending(
      [](const char* type, const String& msg) { g_eventLog.add(type, msg); });
  g_status.lastCrashPresent = CrashRecorder::hasStoredCrash();
  g_status.lastCrashReason = CrashRecorder::lastCrashReason();
  g_cfgMgr.begin();
  g_cfgMgr.load(g_config);
  const BootHealthSnapshot bootHealth = g_cfgMgr.beginBootSession(FIRMWARE_VERSION);
  const bool restoredLastKnownGood = bootHealth.autoRecoveryTriggered && g_cfgMgr.restoreLastKnownGood(g_config);
  const bool explicitRecoveryRequested = g_cfgMgr.consumeRecoveryBootRequest();
  const bool recoveryRequested = explicitRecoveryRequested || bootHealth.autoRecoveryTriggered;
  g_status.consecutiveUnhealthyBoots = bootHealth.consecutiveUnhealthyBoots;
  g_status.autoRecoveryTriggered = bootHealth.autoRecoveryTriggered;
  g_status.lastKnownGoodRestored = restoredLastKnownGood;
  g_status.previousBootDifferentFirmware = bootHealth.previousBootDifferentFirmware;
  if (recoveryRequested) {
    g_status.recoveryMode = true;
  }

  g_eventLog.begin(g_config.eventLogMaxEntries);
  if (bootHealth.previousBootIncomplete) {
    if (bootHealth.previousBootPlannedRestart) {
      String message = "Previous boot ended during a planned restart; strike ignored";
      if (!bootHealth.previousPlannedRestartReason.isEmpty()) {
        message += " (" + bootHealth.previousPlannedRestartReason + ")";
      }
      g_eventLog.add("boot", message);
    } else if (bootHealth.previousBootDifferentFirmware) {
      g_eventLog.add("boot", "Previous boot ended early on prior firmware image; strike ignored across version change");
    } else {
      g_eventLog.add("boot", "Previous boot ended early; count=" + String(bootHealth.consecutiveUnhealthyBoots));
    }
  }
  if (bootHealth.autoRecoveryTriggered) {
    g_eventLog.add("boot", "Auto-recovery triggered after repeated early boot failures");
  }
  if (restoredLastKnownGood) {
    g_eventLog.add("boot", "Restored last-known-good config before entering recovery mode");
  }
  if (!g_status.resetReason.isEmpty()) {
    g_eventLog.add("boot", "Reset reason: " + g_status.resetReason);
  }
  if (crashCaptured) {
    g_eventLog.add("crash", "Crash dump captured from previous boot: " + g_status.lastCrashReason);
  }
  g_status.lastPlannedRestartReason = bootHealth.previousPlannedRestartReason;
  if (!g_status.lastPlannedRestartReason.isEmpty()) {
    g_eventLog.add("boot", "Planned restart breadcrumb: " + g_status.lastPlannedRestartReason);
  }
  g_eventLog.add("boot", recoveryRequested ? "Device booting in recovery mode" : "Device booting");
#ifdef SAFE_FALLBACK_TEST_BAD_BOOT
  g_eventLog.add("boot", recoveryRequested
      ? "Intentional bad-firmware test bypassed in recovery mode"
      : "Intentional bad-firmware test crash armed");
#endif

  g_relay.begin(initialRelayStateFromConfig());
  g_status.relayOn = g_relay.isOn();
  g_led.begin();
  g_button.begin();

  g_wifi.begin(g_config.deviceName, &g_config, explicitRecoveryRequested);
  if (g_wifi.configChangedByPortal()) {
    g_cfgMgr.save(g_config);
    g_eventLog.add("wifi", "Saved Wi-Fi/hub settings entered via setup portal");
  }
  // 0.2.21 (#197): WiFi-state breadcrumb. After g_wifi.begin() so
  // WiFi.SSID() reflects the actually-attempted credentials (pre-fix
  // it always read as <empty> because the WiFi stack hadn't started
  // yet). A future silent loss will surface as
  //   "WiFi state: provisioned_ssid=<empty> operator_networks=0"
  // — without a matching factory-reset breadcrumb earlier in the log,
  // that's the smoking gun for WifiManager flash storage being wiped
  // non-administratively (the .185 06-06 hypothesis).
  {
    const String provisioned = WiFi.SSID();
    g_eventLog.add("boot",
        "WiFi state: provisioned_ssid=" +
        (provisioned.isEmpty() ? String("<empty>") : provisioned) +
        " operator_networks=" + String(g_config.wifi.savedNetworks.size()));
  }
  if (explicitRecoveryRequested && g_wifi.provisionedViaPortal()) {
    g_eventLog.add("boot", "Recovery provisioning completed; rebooting into normal mode");
    g_eventLog.flush();
    g_cfgMgr.markBootHealthy();
    g_cfgMgr.prepareForPlannedRestart("recovery_provisioning_complete");
    delay(150);
    ESP.restart();
  }
  g_notifier.begin(&g_config);
  g_monitor.begin(&g_config, &g_status, &g_relay, &g_notifier, &g_eventLog);
  g_ota.begin(&g_eventLog);
  g_auth.begin(&g_config, &g_eventLog);
  g_central.begin(&g_config, &g_status, &g_cfgMgr, &g_eventLog, &g_relay, &g_wifi, &g_power);
  // 0.2.23 (#179) Phase 3: UDP control listener on port 31416. Same
  // shared secret as the central HTTPS path (deviceToken), no separate
  // provisioning. Idempotent — no-op if WiFi isn't up yet (the socket
  // bind will succeed once the stack is ready).
  UdpControl::begin(&g_relay, &g_eventLog, g_config.central.deviceToken);
  // 0.2.28 #206: diagnostic UDP syslog. Hardcoded collector IP =
  // 192.168.18.11 (the rebooter-droids hub host's LAN address on
  // tmrwww01 — NOT .1 which is the LAN gateway/router). Hardcoded
  // port 51514 matches the hub-side listener in app/services/
  // diag_syslog_collector.py. Hardcoded for this diagnostic build —
  // if we keep the harness past the .185 root-cause investigation,
  // move to a runtime_setting.
  {
    uint8_t macBytes[6] = {0};
    WiFi.macAddress(macBytes);
    char macHex[13];
    snprintf(macHex, sizeof(macHex), "%02x%02x%02x%02x%02x%02x",
             macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
    DiagSyslog::begin(IPAddress(192, 168, 18, 11), 51514, String(macHex));
    // Send the boot's reset_reason immediately so we don't miss it if
    // the device dies before the first heap-snapshot interval fires.
    DiagSyslog::sendResetReason(g_status.resetReason);
    DiagSyslog::sendWifiState("boot_wifi_state", "post-begin");
  }
  g_discovery.begin(&g_config, &g_status);
  g_web.begin(&g_config, &g_status, &g_relay, &g_cfgMgr, &g_eventLog, &g_monitor, &g_ota, &g_auth, &g_wifi, &g_power, &g_discovery);
  g_timeSync.begin(&g_status);
  if (g_wifi.provisionedViaPortal()) {
    // The app that just provisioned us is actively looking on the LAN.
    g_discovery.onPortalProvisioned();
  }

  g_led.setPattern(LedPattern::SlowBlink);
}

void loop() {
  g_status.uptimeSeconds = millis() / 1000;

  g_wifi.loop();
  // 0.2.8 (#154): opt-in non-blocking periodic nearby-network scan. No-op
  // unless wifi.periodicScanEnabled; heap-guarded inside. Mirror the latest
  // summary into status so the heartbeat builder can carry it.
  // 0.2.17 sweep S3: also CLEAR the mirror when the feature is off, so a
  // device that captured a scan and then had the feature disabled stops
  // shipping the stale snapshot in every heartbeat forever.
  g_wifi.loopPeriodicScan(&g_config, ESP.getFreeHeap());
  if (g_config.wifi.periodicScanEnabled) {
    g_status.wifiScanSummary = g_wifi.latestScanSummary();
    g_status.wifiScanUptimeSeconds = g_wifi.latestScanUptimeSeconds();
  } else if (g_status.wifiScanSummary.length() > 0) {
    g_status.wifiScanSummary = "";
    g_status.wifiScanUptimeSeconds = 0;
  }
  g_button.loop();
  g_led.loop();
  g_web.loop();
  g_monitor.loop();
  g_eventLog.loop();

  if (!g_powerStarted && g_config.power.enabled && !g_status.recoveryMode &&
      g_status.uptimeSeconds >= 10 && g_wifi.isConnected()) {
    g_power.begin(&g_status);
    g_powerStarted = true;
  }
  if (g_powerStarted) {
    g_power.loop();
  }

  g_central.loop();
  UdpControl::loop();  // 0.2.23 (#179): drain UDP control packets
  DiagSyslog::loop();  // 0.2.27 (#206): periodic heap snapshot to diag
  g_discovery.loop();

  g_status.wifiConnected = g_wifi.isConnected();
  g_status.inCaptivePortal = g_wifi.inCaptivePortal();
  g_status.setupApName = g_wifi.setupApName();
  g_status.relayOn = g_relay.isOn();
  g_timeSync.loop(g_status.wifiConnected);

  const uint32_t healthyAfterSeconds = g_status.recoveryMode ? 15UL : 90UL;
  if (!g_bootMarkedHealthy && g_status.uptimeSeconds >= healthyAfterSeconds) {
    if (g_cfgMgr.markBootHealthy()) {
      g_bootMarkedHealthy = true;
      g_status.bootHealthyMarked = true;
      g_status.consecutiveUnhealthyBoots = 0;
      g_status.autoRecoveryTriggered = false;
      g_eventLog.add("boot", "Boot marked healthy");
    }
  }

  maybeTriggerBadBootTest();

  if (g_button.shortPressed() && g_config.currentMode == DeviceMode::SmartPlug && g_config.manualButtonEnabled) {
    g_relay.toggle();
    persistManualRelayState();
    g_eventLog.add("relay", g_relay.isOn() ? "Relay turned on by button" : "Relay turned off by button");
  }

  if (g_button.longPressed3s()) {
    g_eventLog.add("system", "Reboot requested by button");
    g_eventLog.flush();
    prepareForPlannedRestart("button_reboot");
    ESP.restart();
  }

  if (g_button.longPressed10s()) {
    enterRecoveryMode();
  }

  if (g_button.longPressed30s()) {
    g_eventLog.add("system", "Factory reset requested by button");
    g_eventLog.flush();
    g_wifi.clearProvisionedCredentials();
    CrashRecorder::clearStoredCrashes();
    g_cfgMgr.reset();
    g_cfgMgr.prepareForPlannedRestart("button_factory_reset");
    ESP.restart();
  }
}
