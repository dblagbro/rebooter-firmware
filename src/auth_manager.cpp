#include <Arduino.h>
#include <Hash.h>
#include "auth_manager.h"
#include "event_log.h"

void AuthManager::begin(AppConfig* config, EventLog* eventLog) {
  config_ = config;
  eventLog_ = eventLog;
}

bool AuthManager::isProvisioned() const {
  return config_ && !config_->adminPasswordHash.isEmpty() && !config_->adminPasswordSalt.isEmpty();
}

bool AuthManager::setPassword(const String& username, const String& password) {
  if (!config_) return false;

  String cleanUser = username;
  cleanUser.trim();
  if (cleanUser.isEmpty() || cleanUser.length() > 32) return false;
  if (password.length() < 8 || password.length() > 64) return false;

  config_->adminUsername = cleanUser;
  config_->adminPasswordSalt = String(ESP.getChipId(), HEX) + "-" + String(micros(), HEX);
  config_->adminPasswordHash = hashPassword(password);
  if (eventLog_) eventLog_->add("auth", "Admin credentials updated");
  return true;
}

bool AuthManager::isAuthorized(ESP8266WebServer& server) {
  if (!isProvisioned()) return true;

  String token = server.header("X-Rebooter-Auth");
  token.trim();
  return !token.isEmpty() && hashPassword(token) == config_->adminPasswordHash;
}

bool AuthManager::requireAuth(ESP8266WebServer& server) {
  if (isAuthorized(server)) return true;
  reject(server);
  return false;
}

String AuthManager::hashPassword(const String& password) const {
  if (!config_) return "";
  return sha1(config_->adminPasswordSalt + ":" + password);
}

void AuthManager::reject(ESP8266WebServer& server) {
  server.sendHeader("WWW-Authenticate", "Rebooter token");
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
}