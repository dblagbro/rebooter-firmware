#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include "notification_manager.h"

void NotificationManager::begin(AppConfig* config) {
  config_ = config;
}

bool NotificationManager::send(const String& eventType, const String& reason, const String& detailsJson) {
  if (!config_ || !config_->notifications.enabled || config_->notifications.webhookUrl.isEmpty()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, config_->notifications.webhookUrl)) return false;
  http.addHeader("Content-Type", "application/json");
  if (!config_->notifications.webhookAuthToken.isEmpty()) {
    http.addHeader("Authorization", "Bearer " + config_->notifications.webhookAuthToken);
  }

  String body = "{\"event_type\":\"" + eventType + "\",\"reason\":\"" + reason + "\",\"details\":" + detailsJson + "}";
  int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

