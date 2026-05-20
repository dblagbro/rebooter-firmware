#include "discovery_manager.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "firmware_version.h"
#include "status_payload.h"

namespace {
// Post-portal-provisioning announce window: the app has just moved from the
// device's own AP onto the user's Wi-Fi and is actively hunting for it.
constexpr uint32_t PORTAL_ANNOUNCE_WINDOW_MS = 120000;
constexpr uint32_t PORTAL_ANNOUNCE_INTERVAL_MS = 5000;
// Normal-boot burst: a short 3-packet announce so an app can re-find the
// device after a reboot.
constexpr uint32_t BOOT_ANNOUNCE_WINDOW_MS = 6000;
constexpr uint32_t BOOT_ANNOUNCE_INTERVAL_MS = 2000;
// On-demand burst triggered via the API.
constexpr uint32_t ONDEMAND_ANNOUNCE_WINDOW_MS = 6000;
constexpr uint32_t ONDEMAND_ANNOUNCE_INTERVAL_MS = 2000;
}  // namespace

void DiscoveryManager::begin(AppConfig* config, RuntimeStatus* status) {
  config_ = config;
  status_ = status;
  mdnsActive_ = false;
  wasConnected_ = false;
  announceUntilMs_ = 0;
}

String DiscoveryManager::hostname_() const {
  // rebooter-<last 6 hex of chip id>
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06x", ESP.getChipId() & 0xFFFFFF);
  return String("rebooter-") + suffix;
}

void DiscoveryManager::startMdns_() {
  if (mdnsActive_ || !config_) return;
  if (!config_->discovery.mdnsEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!MDNS.begin(hostname_())) return;
  mdnsActive_ = true;

  MDNS.addService("rebooter", "tcp", 80);
  MDNS.addServiceTxt("rebooter", "tcp", "id", hostname_().substring(9));
  MDNS.addServiceTxt("rebooter", "tcp", "name", config_->deviceName);
  MDNS.addServiceTxt("rebooter", "tcp", "fw", FIRMWARE_VERSION);
  MDNS.addServiceTxt("rebooter", "tcp", "mode",
                     StatusPayload::modeToString(config_->currentMode));
  MDNS.addServiceTxt("rebooter", "tcp", "auth",
                     config_->adminPasswordHash.isEmpty() ? "0" : "1");
}

String DiscoveryManager::buildAnnouncePayload_() const {
  // Small JSON, well under one MTU. No secret/token is ever included; this
  // is plaintext on the LAN.
  String body = "{\"t\":\"rebooter-beacon\",\"v\":1";
  body += ",\"id\":\"" + hostname_().substring(9) + "\"";
  body += ",\"name\":\"" + (config_ ? config_->deviceName : String("Rebooter")) + "\"";
  body += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  body += ",\"fw\":\"" + String(FIRMWARE_VERSION) + "\"";
  body += ",\"mode\":\"" +
          (config_ ? StatusPayload::modeToString(config_->currentMode)
                   : String("smart_plug")) +
          "\"";
  body += ",\"api\":80";
  body += ",\"auth_set\":";
  body += (config_ && !config_->adminPasswordHash.isEmpty()) ? "true" : "false";
  body += "}";
  return body;
}

void DiscoveryManager::sendAnnouncePacket_() {
  if (!config_ || WiFi.status() != WL_CONNECTED) return;

  IPAddress local = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  // Directed subnet broadcast address.
  IPAddress broadcast(local[0] | ~mask[0], local[1] | ~mask[1],
                      local[2] | ~mask[2], local[3] | ~mask[3]);

  const String payload = buildAnnouncePayload_();
  if (udp_.beginPacket(broadcast, config_->discovery.udpPort)) {
    udp_.write(reinterpret_cast<const uint8_t*>(payload.c_str()),
               payload.length());
    udp_.endPacket();
  }
}

void DiscoveryManager::onWifiConnected() {
  if (!config_) return;
  startMdns_();
  // Short boot burst so an app can re-discover the device after a reboot.
  if (config_->discovery.udpAnnounceEnabled && announceUntilMs_ == 0) {
    announceUntilMs_ = millis() + BOOT_ANNOUNCE_WINDOW_MS;
    announceIntervalMs_ = BOOT_ANNOUNCE_INTERVAL_MS;
    nextAnnounceAtMs_ = millis();
  }
}

void DiscoveryManager::onPortalProvisioned() {
  if (!config_ || !config_->discovery.udpAnnounceEnabled) return;
  // The longer post-setup window overrides any boot burst.
  announceUntilMs_ = millis() + PORTAL_ANNOUNCE_WINDOW_MS;
  announceIntervalMs_ = PORTAL_ANNOUNCE_INTERVAL_MS;
  nextAnnounceAtMs_ = millis();
}

void DiscoveryManager::triggerAnnounceBurst() {
  if (!config_ || !config_->discovery.udpAnnounceEnabled) return;
  announceUntilMs_ = millis() + ONDEMAND_ANNOUNCE_WINDOW_MS;
  announceIntervalMs_ = ONDEMAND_ANNOUNCE_INTERVAL_MS;
  nextAnnounceAtMs_ = millis();
}

void DiscoveryManager::loop() {
  if (!config_) return;

  const bool connected = WiFi.status() == WL_CONNECTED;

  // Detect a fresh Wi-Fi-up edge so reconnects also restart mDNS / burst.
  if (connected && !wasConnected_) {
    onWifiConnected();
  }
  if (!connected && wasConnected_) {
    mdnsActive_ = false;  // responder is gone with the link
  }
  wasConnected_ = connected;

  if (!connected) return;

  if (mdnsActive_) {
    MDNS.update();
  } else if (config_->discovery.mdnsEnabled) {
    // Enabled but not yet up (e.g. toggled on at runtime): start it.
    startMdns_();
  }

  // Time-bounded UDP announce burst.
  if (announceUntilMs_ != 0) {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - announceUntilMs_) >= 0) {
      announceUntilMs_ = 0;  // window closed; go silent
    } else if (static_cast<int32_t>(now - nextAnnounceAtMs_) >= 0) {
      sendAnnouncePacket_();
      nextAnnounceAtMs_ = now + announceIntervalMs_;
    }
  }
}
