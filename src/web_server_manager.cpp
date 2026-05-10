#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "web_server_manager.h"
#include "relay_controller.h"
#include "config_manager.h"
#include "event_log.h"
#include "monitor_engine.h"
#include "ota_manager.h"
#include "auth_manager.h"

static const char FALLBACK_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Rebooter</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <main class="shell">
    <header class="topbar">
      <div>
        <h1>Rebooter</h1>
        <p id="connection-note">Connecting to device...</p>
      </div>
      <div class="status-pill" id="health-pill">Unknown</div>
    </header>

    <section class="band grid-two">
      <article class="panel">
        <h2>Device</h2>
        <dl class="kv">
          <div><dt>Name</dt><dd id="device-name">-</dd></div>
          <div><dt>Mode</dt><dd id="device-mode">-</dd></div>
          <div><dt>Relay</dt><dd id="relay-state">-</dd></div>
          <div><dt>Wi-Fi</dt><dd id="wifi-state">-</dd></div>
          <div><dt>IP</dt><dd id="device-ip">-</dd></div>
          <div><dt>Uptime</dt><dd id="uptime">-</dd></div>
        </dl>
      </article>

      <article class="panel">
        <h2>Relay</h2>
        <div class="actions">
          <button id="relay-on">Turn On</button>
          <button id="relay-off">Turn Off</button>
          <button id="relay-toggle">Toggle</button>
          <button id="refresh-status" class="secondary">Refresh</button>
        </div>
        <p class="hint">Relay commands require auth only after you set an admin password.</p>
      </article>
    </section>

    <section class="band grid-two">
      <article class="panel">
        <h2>Provisioning</h2>
        <form id="config-form" class="stack">
          <label><span>Device Name</span><input id="cfg-device-name" maxlength="32"></label>
          <label>
            <span>Mode</span>
            <select id="cfg-mode">
              <option value="smart_plug">Smart Plug</option>
              <option value="internet_watchdog">Internet Watchdog</option>
              <option value="device_watchdog">Device Watchdog</option>
            </select>
          </label>
          <label>
            <span>Relay Restore</span>
            <select id="cfg-restore">
              <option value="restore_previous">Restore Previous</option>
              <option value="always_on">Always On</option>
              <option value="always_off">Always Off</option>
            </select>
          </label>
          <label><span>Monitor Interval (sec)</span><input id="cfg-monitor-interval" type="number" min="2" max="3600"></label>
          <label><span>Boot Warm-up (sec)</span><input id="cfg-boot-warmup" type="number" min="0" max="600"></label>
          <label class="checkbox-row"><input id="cfg-manual-button" type="checkbox"><span>Enable short button press for relay in Smart Plug mode</span></label>
          <label><span>Admin Username</span><input id="cfg-admin-username" maxlength="32" placeholder="admin"></label>
          <label><span>Admin Password</span><input id="cfg-admin-password" type="password" minlength="8" maxlength="64" placeholder="Leave blank to keep current"></label>
          <div class="actions"><button type="submit">Save Settings</button></div>
        </form>
      </article>

      <article class="panel">
        <h2>Firmware Update</h2>
        <form id="ota-form" class="stack">
          <label><span>Firmware Bin</span><input id="ota-file" type="file" accept=".bin,application/octet-stream"></label>
          <div class="actions"><button type="submit">Upload Firmware</button></div>
        </form>
        <div class="progress-wrap">
          <progress id="ota-progress" max="100" value="0"></progress>
          <span id="ota-progress-label">Idle</span>
        </div>
        <p class="hint">This uses the device's local OTA endpoint. No serial cable needed after bootstrap.</p>
      </article>
    </section>

    <section class="band"><article class="panel"><h2>Events</h2><pre id="events-view">Loading events...</pre></article></section>
    <section class="band"><article class="panel"><h2>Message Log</h2><pre id="message-log">Ready.</pre></article></section>
  </main>
  <script src="/app.js"></script>
</body>
</html>
)HTML";

static const char FALLBACK_STYLE_CSS[] PROGMEM = R"CSS(
* { box-sizing: border-box; }
body { margin: 0; font-family: Arial, sans-serif; background: #eef2f0; color: #17211d; }
.shell { max-width: 1080px; margin: 0 auto; padding: 20px; }
.topbar { display: flex; justify-content: space-between; align-items: flex-start; gap: 16px; padding: 20px 0 8px; }
.topbar h1 { margin: 0 0 8px; font-size: 34px; }
.topbar p { margin: 0; color: #54625c; }
.band { margin-top: 18px; }
.grid-two { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 18px; }
.panel { background: #ffffff; border: 1px solid #cfd8d4; border-radius: 8px; padding: 18px; }
.panel h2 { margin: 0 0 14px; font-size: 20px; }
.kv { margin: 0; }
.kv div { display: flex; justify-content: space-between; gap: 16px; padding: 8px 0; border-bottom: 1px solid #edf2ef; }
.kv div:last-child { border-bottom: 0; }
.kv dt { font-weight: 700; }
.kv dd { margin: 0; text-align: right; color: #42514b; }
.actions { display: flex; flex-wrap: wrap; gap: 10px; }
button, input, select { font: inherit; }
button { appearance: none; border: 1px solid #124d39; background: #124d39; color: #ffffff; border-radius: 8px; padding: 10px 14px; cursor: pointer; }
button.secondary { background: #ffffff; color: #124d39; }
.stack { display: grid; gap: 12px; }
label { display: grid; gap: 6px; }
label span { font-size: 14px; color: #42514b; }
input, select { width: 100%; border: 1px solid #b8c5c0; border-radius: 8px; padding: 10px 12px; background: #ffffff; color: #17211d; }
.checkbox-row { grid-template-columns: auto 1fr; align-items: center; column-gap: 10px; }
.checkbox-row input { width: auto; }
.status-pill { min-width: 116px; text-align: center; padding: 10px 12px; border-radius: 8px; font-weight: 700; background: #d8e0dc; color: #21312b; }
.status-pill.healthy { background: #d8f0da; color: #1e5c27; }
.status-pill.partial-failure, .status-pill.holdoff { background: #f3e4b9; color: #6d5614; }
.status-pill.failed, .status-pill.cooldown { background: #f3c7bf; color: #7b2319; }
.progress-wrap { display: grid; gap: 8px; margin-top: 14px; }
progress { width: 100%; height: 16px; }
.hint { margin: 10px 0 0; color: #5d6c66; font-size: 14px; }
pre { margin: 0; padding: 14px; background: #f6f9f7; border: 1px solid #dee7e2; border-radius: 8px; white-space: pre-wrap; overflow-wrap: anywhere; }
@media (max-width: 800px) { .grid-two { grid-template-columns: 1fr; } .topbar { flex-direction: column; } .topbar h1 { font-size: 28px; } }
)CSS";

static const char FALLBACK_APP_JS[] PROGMEM = R"JS(
const state={status:null,config:null};const $=id=>document.getElementById(id);function logMessage(message){const view=$('message-log');const stamp=new Date().toLocaleTimeString();view.textContent=`[${stamp}] ${message}\n`+view.textContent}function formatUptime(seconds){const s=Number(seconds||0);const d=Math.floor(s/86400);const h=Math.floor((s%86400)/3600);const m=Math.floor((s%3600)/60);const rem=s%60;return `${d}d ${h}h ${m}m ${rem}s`}function setHealthPill(health){const pill=$('health-pill');pill.textContent=health||'unknown';pill.className=`status-pill ${String(health||'unknown').replace(/_/g,'-')}`}function renderStatus(){if(!state.status)return;$('device-name').textContent=state.status.device_name||'-';$('device-mode').textContent=state.status.mode||'-';$('relay-state').textContent=state.status.relay_on?'On':'Off';$('wifi-state').textContent=state.status.wifi_connected?'Connected':'Disconnected';$('device-ip').textContent=window.location.host||'-';$('uptime').textContent=formatUptime(state.status.uptime_seconds);$('connection-note').textContent=state.status.wifi_connected?'Device is online and serving the local control plane.':'Device is up but Wi-Fi is not currently connected.';setHealthPill(state.status.health_state)}function renderConfig(){if(!state.config)return;$('cfg-device-name').value=state.config.device_name||'';$('cfg-mode').value=state.config.current_mode||'smart_plug';$('cfg-restore').value=state.config.relay_restore_behavior||'restore_previous';$('cfg-monitor-interval').value=state.config.monitor_interval_seconds??5;$('cfg-boot-warmup').value=state.config.boot_warmup_seconds??30;$('cfg-manual-button').checked=!!state.config.manual_button_enabled;$('cfg-admin-username').value=state.config.admin_username||'admin'}async function fetchJson(path,options){const response=await fetch(path,options);const text=await response.text();let json={};if(text){try{json=JSON.parse(text)}catch(_){json={raw:text}}}if(!response.ok){throw new Error(json.error||`${response.status} ${response.statusText}`)}return json}async function refreshStatus(){state.status=await fetchJson('/api/status');renderStatus()}async function refreshConfig(){state.config=await fetchJson('/api/config');renderConfig()}async function refreshEvents(){const events=await fetchJson('/api/events');$('events-view').textContent=JSON.stringify(events,null,2)}async function refreshAll(){try{await Promise.all([refreshStatus(),refreshConfig(),refreshEvents()])}catch(error){logMessage(`Refresh failed: ${error.message}`)}}async function postJson(path,payload){return fetchJson(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})}async function handleRelay(path,label){try{await fetchJson(path,{method:'POST'});logMessage(`${label} command sent.`);await refreshStatus()}catch(error){logMessage(`${label} failed: ${error.message}`)}}async function handleConfigSave(event){event.preventDefault();const payload={device_name:$('cfg-device-name').value.trim(),current_mode:$('cfg-mode').value,relay_restore_behavior:$('cfg-restore').value,monitor_interval_seconds:Number($('cfg-monitor-interval').value||5),boot_warmup_seconds:Number($('cfg-boot-warmup').value||30),manual_button_enabled:$('cfg-manual-button').checked,admin_username:$('cfg-admin-username').value.trim()};const password=$('cfg-admin-password').value;if(password)payload.admin_password=password;if(state.config?.internet)payload.internet=state.config.internet;if(state.config?.device)payload.device=state.config.device;if(state.config?.notifications)payload.notifications=state.config.notifications;try{await postJson('/api/config/save',payload);$('cfg-admin-password').value='';logMessage('Settings saved.');await refreshAll()}catch(error){logMessage(`Save failed: ${error.message}`)}}async function handleOtaSubmit(event){event.preventDefault();const input=$('ota-file');const file=input.files&&input.files[0];if(!file){logMessage('Choose a firmware .bin first.');return}const formData=new FormData();formData.append('update',file);const xhr=new XMLHttpRequest();xhr.open('POST','/api/system/ota',true);xhr.upload.onprogress=evt=>{if(!evt.lengthComputable)return;const percent=Math.round((evt.loaded/evt.total)*100);$('ota-progress').value=percent;$('ota-progress-label').textContent=`${percent}%`};xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){$('ota-progress').value=100;$('ota-progress-label').textContent='Upload complete, device rebooting';logMessage('OTA upload accepted. Device is rebooting.')}else{$('ota-progress-label').textContent=`Failed (${xhr.status})`;logMessage(`OTA upload failed: ${xhr.responseText||xhr.status}`)}};xhr.onerror=()=>{$('ota-progress-label').textContent='Network error';logMessage('OTA upload hit a network error.')};$('ota-progress').value=0;$('ota-progress-label').textContent='Uploading...';logMessage(`Uploading firmware: ${file.name}`);xhr.send(formData)}function wireUi(){$('relay-on').addEventListener('click',()=>handleRelay('/api/relay/on','Relay on'));$('relay-off').addEventListener('click',()=>handleRelay('/api/relay/off','Relay off'));$('relay-toggle').addEventListener('click',()=>handleRelay('/api/relay/toggle','Relay toggle'));$('refresh-status').addEventListener('click',refreshAll);$('config-form').addEventListener('submit',handleConfigSave);$('ota-form').addEventListener('submit',handleOtaSubmit)}wireUi();refreshAll();setInterval(refreshStatus,5000);
)JS";

static ESP8266WebServer server(80);
static RelayController* sRelay = nullptr;
static AppConfig* sConfig = nullptr;
static RuntimeStatus* sStatus = nullptr;
static ConfigManager* sCfgMgr = nullptr;
static EventLog* sEventLog = nullptr;
static MonitorEngine* sMonitor = nullptr;
static OtaManager* sOta = nullptr;
static AuthManager* sAuth = nullptr;

static bool parseBaseUrl(const String& baseUrl, String& host, uint16_t& port, String& rootPath) {
  String url = baseUrl;
  url.trim();
  if (url.endsWith("/")) url.remove(url.length() - 1);

  port = 443;
  if (url.startsWith("https://")) {
    url.remove(0, 8);
  } else if (url.startsWith("http://")) {
    url.remove(0, 7);
    port = 80;
  } else {
    return false;
  }

  const int slash = url.indexOf('/');
  String hostPort = slash >= 0 ? url.substring(0, slash) : url;
  rootPath = slash >= 0 ? url.substring(slash) : "/";
  if (rootPath.isEmpty()) rootPath = "/";

  const int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
  } else {
    host = hostPort;
  }

  return !host.isEmpty() && port > 0;
}

static void addCentralDiagnostic(JsonObject target, const String& baseUrl) {
  target["base_url"] = baseUrl;
  target["free_heap"] = ESP.getFreeHeap();

  String host;
  String rootPath;
  uint16_t port = 0;
  if (!parseBaseUrl(baseUrl, host, port, rootPath)) {
    target["parse_ok"] = false;
    return;
  }

  target["parse_ok"] = true;
  target["host"] = host;
  target["port"] = port;
  target["root_path"] = rootPath;

  IPAddress resolved;
  const bool dnsOk = WiFi.hostByName(host.c_str(), resolved);
  target["dns_ok"] = dnsOk;
  target["resolved_ip"] = dnsOk ? resolved.toString() : "";

  WiFiClient tcp;
  tcp.setTimeout(5000);
  const bool tcpOk = dnsOk ? tcp.connect(resolved, port) : tcp.connect(host.c_str(), port);
  target["tcp_connect_ok"] = tcpOk;
  if (tcpOk) tcp.stop();

  std::unique_ptr<BearSSL::WiFiClientSecure> secure(new BearSSL::WiFiClientSecure());
  secure->setInsecure();
  secure->setBufferSizes(512, 512);
  HTTPClient http;
  const String versionUrl = baseUrl + (baseUrl.endsWith("/") ? "" : "/") + "api/v1/version";
  const bool beginOk = http.begin(*secure, versionUrl);
  target["https_begin_ok"] = beginOk;
  target["version_url"] = versionUrl;
  if (!beginOk) {
    return;
  }

  const int code = http.GET();
  target["https_code"] = code;
  if (code < 0) {
    target["https_error"] = HTTPClient::errorToString(code);
  } else {
    const String body = http.getString();
    target["https_body"] = body.substring(0, min(static_cast<int>(body.length()), 180));
  }
  http.end();
}

static void serveFileOrFallback(const char* path, const char* contentType, PGM_P fallback) {
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    server.streamFile(f, contentType);
    f.close();
    return;
  }

  server.send_P(200, contentType, fallback);
}

static String modeToString(DeviceMode mode) {
  switch (mode) {
    case DeviceMode::InternetWatchdog: return "internet_watchdog";
    case DeviceMode::DeviceWatchdog: return "device_watchdog";
    default: return "smart_plug";
  }
}

static String healthToString(HealthState state) {
  switch (state) {
    case HealthState::Healthy: return "healthy";
    case HealthState::PartialFailure: return "partial_failure";
    case HealthState::Failed: return "failed";
    case HealthState::Holdoff: return "holdoff";
    case HealthState::Cooldown: return "cooldown";
    default: return "unknown";
  }
}

static String restoreToString(RelayRestoreBehavior value) {
  switch (value) {
    case RelayRestoreBehavior::AlwaysOn: return "always_on";
    case RelayRestoreBehavior::AlwaysOff: return "always_off";
    default: return "restore_previous";
  }
}

static RelayRestoreBehavior restoreFromString(const String& value) {
  if (value == "always_on") return RelayRestoreBehavior::AlwaysOn;
  if (value == "always_off") return RelayRestoreBehavior::AlwaysOff;
  return RelayRestoreBehavior::RestorePrevious;
}

static void persistRelayState() {
  sConfig->lastRelayOn = sRelay->isOn();
  sCfgMgr->save(*sConfig);
}

static bool requireAuth() {
  return sAuth && sAuth->requireAuth(server);
}

static void sendConfigJson() {
  JsonDocument doc;
  doc["schema_version"] = sConfig->schemaVersion;
  doc["device_name"] = sConfig->deviceName;
  doc["admin_username"] = sConfig->adminUsername;
  doc["current_mode"] = modeToString(sConfig->currentMode);
  doc["relay_restore_behavior"] = restoreToString(sConfig->relayRestoreBehavior);
  doc["last_relay_on"] = sConfig->lastRelayOn;
  doc["monitor_interval_seconds"] = sConfig->monitorIntervalSeconds;
  doc["boot_warmup_seconds"] = sConfig->bootWarmupSeconds;
  doc["manual_button_enabled"] = sConfig->manualButtonEnabled;

  JsonArray targets = doc["internet"]["targets"].to<JsonArray>();
  for (const auto& target : sConfig->internet.targets) targets.add(target);
  doc["internet"]["failure_threshold_seconds"] = sConfig->internet.failureThresholdSeconds;
  doc["internet"]["power_off_seconds"] = sConfig->internet.powerOffSeconds;
  doc["internet"]["post_reboot_holdoff_seconds"] = sConfig->internet.postRebootHoldoffSeconds;
  doc["internet"]["max_cycles_per_incident"] = sConfig->internet.maxCyclesPerIncident;
  doc["internet"]["max_cycles_per_hour"] = sConfig->internet.maxCyclesPerHour;
  doc["internet"]["cooldown_seconds"] = sConfig->internet.cooldownSeconds;

  doc["device"]["target"] = sConfig->device.target;
  doc["device"]["failure_threshold_seconds"] = sConfig->device.failureThresholdSeconds;
  doc["device"]["power_off_seconds"] = sConfig->device.powerOffSeconds;
  doc["device"]["post_reboot_holdoff_seconds"] = sConfig->device.postRebootHoldoffSeconds;
  doc["device"]["max_cycles_per_incident"] = sConfig->device.maxCyclesPerIncident;
  doc["device"]["max_cycles_per_hour"] = sConfig->device.maxCyclesPerHour;
  doc["device"]["cooldown_seconds"] = sConfig->device.cooldownSeconds;

  doc["central"]["enabled"] = sConfig->central.enabled;
  JsonArray centralBaseUrls = doc["central"]["base_urls"].to<JsonArray>();
  for (const auto& url : sConfig->central.baseUrls) centralBaseUrls.add(url);
  doc["central"]["enrollment_token"] = sConfig->central.enrollmentToken;
  doc["central"]["device_alias"] = sConfig->central.deviceAlias;
  doc["central"]["site_id"] = sConfig->central.siteId;
  doc["central"]["device_id"] = sConfig->central.deviceId;
  doc["central"]["poll_interval_seconds"] = sConfig->central.pollIntervalSeconds;
  doc["central"]["heartbeat_interval_seconds"] = sConfig->central.heartbeatIntervalSeconds;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void WebServerManager::begin(AppConfig* config, RuntimeStatus* status,
                             RelayController* relay, ConfigManager* cfgMgr,
                             EventLog* eventLog, MonitorEngine* monitor,
                             OtaManager* ota,
                             AuthManager* auth) {
  config_ = config;
  status_ = status;
  sConfig = config;
  sStatus = status;
  sRelay = relay;
  sCfgMgr = cfgMgr;
  sEventLog = eventLog;
  sMonitor = monitor;
  sOta = ota;
  sAuth = auth;
  server.collectHeaders("X-Rebooter-Auth");

  server.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["device_name"] = sConfig->deviceName;
    doc["mode"] = modeToString(sConfig->currentMode);
    doc["relay_on"] = sRelay->isOn();
    doc["wifi_connected"] = sStatus->wifiConnected;
    doc["health_state"] = healthToString(sStatus->healthState);
    doc["uptime_seconds"] = sStatus->uptimeSeconds;
    doc["incident_cycles"] = sStatus->currentIncidentCycles;
    doc["hour_cycles"] = sStatus->currentHourCycles;
    doc["holdoff_remaining_seconds"] = sStatus->holdoffRemainingSeconds;
    doc["cooldown_remaining_seconds"] = sStatus->cooldownRemainingSeconds;
    doc["central_enabled"] = sStatus->centralEnabled;
    doc["central_registered"] = sStatus->centralRegistered;
    doc["central_state"] = sStatus->centralState;
    doc["central_device_id"] = sStatus->centralDeviceId;
    doc["central_last_heartbeat_seconds"] = sStatus->centralLastHeartbeatSeconds;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/config", HTTP_GET, []() {
    sendConfigJson();
  });

  server.on("/api/events", HTTP_GET, []() {
    server.send(200, "application/json", sEventLog->asJson());
  });

  server.on("/api/system/central-diagnostic", HTTP_GET, []() {
    if (!requireAuth()) return;

    JsonDocument doc;
    doc["wifi_connected"] = sStatus->wifiConnected;
    doc["local_ip"] = WiFi.localIP().toString();
    doc["mac_address"] = WiFi.macAddress();
    doc["free_heap"] = ESP.getFreeHeap();

    JsonArray diagnostics = doc["targets"].to<JsonArray>();
    for (const auto& baseUrl : sConfig->central.baseUrls) {
      JsonObject entry = diagnostics.add<JsonObject>();
      addCentralDiagnostic(entry, baseUrl);
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/relay/on", HTTP_POST, []() {
    if (!requireAuth()) return;
    sRelay->set(true);
    persistRelayState();
    sEventLog->add("relay", "Relay turned on by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/off", HTTP_POST, []() {
    if (!requireAuth()) return;
    sRelay->set(false);
    persistRelayState();
    sEventLog->add("relay", "Relay turned off by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/toggle", HTTP_POST, []() {
    if (!requireAuth()) return;
    sRelay->toggle();
    persistRelayState();
    sEventLog->add("relay", sRelay->isOn() ? "Relay toggled on by API" : "Relay toggled off by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/config/save", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"missing body\"}");
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      server.send(400, "application/json", "{\"error\":\"invalid json\"}");
      return;
    }

    const String mode = doc["current_mode"] | modeToString(sConfig->currentMode);
    if (mode == "internet_watchdog") sConfig->currentMode = DeviceMode::InternetWatchdog;
    else if (mode == "device_watchdog") sConfig->currentMode = DeviceMode::DeviceWatchdog;
    else sConfig->currentMode = DeviceMode::SmartPlug;

    const String restore = doc["relay_restore_behavior"] | restoreToString(sConfig->relayRestoreBehavior);
    sConfig->relayRestoreBehavior = restoreFromString(restore);
    sConfig->deviceName = doc["device_name"] | sConfig->deviceName;
    if (doc["admin_password"].is<const char*>()) {
      const String user = doc["admin_username"] | sConfig->adminUsername;
      const String password = doc["admin_password"] | "";
      if (!sAuth->setPassword(user, password)) {
        server.send(400, "application/json", "{\"error\":\"invalid credentials\"}");
        return;
      }
    }
    sConfig->monitorIntervalSeconds = doc["monitor_interval_seconds"] | sConfig->monitorIntervalSeconds;
    sConfig->bootWarmupSeconds = doc["boot_warmup_seconds"] | sConfig->bootWarmupSeconds;
    sConfig->manualButtonEnabled = doc["manual_button_enabled"] | sConfig->manualButtonEnabled;

    if (doc["internet"]["targets"].is<JsonArray>()) {
      sConfig->internet.targets.clear();
      for (JsonVariant v : doc["internet"]["targets"].as<JsonArray>()) sConfig->internet.targets.push_back(String((const char*)v));
    }
    sConfig->internet.failureThresholdSeconds = doc["internet"]["failure_threshold_seconds"] | sConfig->internet.failureThresholdSeconds;
    sConfig->internet.powerOffSeconds = doc["internet"]["power_off_seconds"] | sConfig->internet.powerOffSeconds;
    sConfig->internet.postRebootHoldoffSeconds = doc["internet"]["post_reboot_holdoff_seconds"] | sConfig->internet.postRebootHoldoffSeconds;
    sConfig->internet.maxCyclesPerIncident = doc["internet"]["max_cycles_per_incident"] | sConfig->internet.maxCyclesPerIncident;
    sConfig->internet.maxCyclesPerHour = doc["internet"]["max_cycles_per_hour"] | sConfig->internet.maxCyclesPerHour;
    sConfig->internet.cooldownSeconds = doc["internet"]["cooldown_seconds"] | sConfig->internet.cooldownSeconds;
    sConfig->internet.dnsRefreshSeconds = doc["internet"]["dns_refresh_seconds"] | sConfig->internet.dnsRefreshSeconds;
    sConfig->internet.recoveryStabilitySeconds = doc["internet"]["recovery_stability_seconds"] | sConfig->internet.recoveryStabilitySeconds;

    sConfig->device.target = doc["device"]["target"] | sConfig->device.target;
    sConfig->device.failureThresholdSeconds = doc["device"]["failure_threshold_seconds"] | sConfig->device.failureThresholdSeconds;
    sConfig->device.powerOffSeconds = doc["device"]["power_off_seconds"] | sConfig->device.powerOffSeconds;
    sConfig->device.postRebootHoldoffSeconds = doc["device"]["post_reboot_holdoff_seconds"] | sConfig->device.postRebootHoldoffSeconds;
    sConfig->device.maxCyclesPerIncident = doc["device"]["max_cycles_per_incident"] | sConfig->device.maxCyclesPerIncident;
    sConfig->device.maxCyclesPerHour = doc["device"]["max_cycles_per_hour"] | sConfig->device.maxCyclesPerHour;
    sConfig->device.cooldownSeconds = doc["device"]["cooldown_seconds"] | sConfig->device.cooldownSeconds;
    sConfig->device.recoveryStabilitySeconds = doc["device"]["recovery_stability_seconds"] | sConfig->device.recoveryStabilitySeconds;

    sConfig->notifications.enabled = doc["notifications"]["enabled"] | sConfig->notifications.enabled;
    sConfig->notifications.webhookUrl = doc["notifications"]["webhook_url"] | sConfig->notifications.webhookUrl;
    sConfig->notifications.webhookAuthToken = doc["notifications"]["webhook_auth_token"] | sConfig->notifications.webhookAuthToken;

    const String previousEnrollmentToken = sConfig->central.enrollmentToken;
    sConfig->central.enabled = doc["central"]["enabled"] | sConfig->central.enabled;
    if (doc["central"]["base_urls"].is<JsonArray>()) {
      sConfig->central.baseUrls.clear();
      for (JsonVariant v : doc["central"]["base_urls"].as<JsonArray>()) sConfig->central.baseUrls.push_back(String((const char*)v));
    } else if (doc["central"]["base_url"].is<const char*>()) {
      sConfig->central.baseUrls.clear();
      sConfig->central.baseUrls.push_back(String((const char*)doc["central"]["base_url"]));
    }
    sConfig->central.enrollmentToken = doc["central"]["enrollment_token"] | sConfig->central.enrollmentToken;
    sConfig->central.deviceAlias = doc["central"]["device_alias"] | sConfig->central.deviceAlias;
    sConfig->central.siteId = doc["central"]["site_id"] | sConfig->central.siteId;
    sConfig->central.pollIntervalSeconds = doc["central"]["poll_interval_seconds"] | sConfig->central.pollIntervalSeconds;
    sConfig->central.heartbeatIntervalSeconds = doc["central"]["heartbeat_interval_seconds"] | sConfig->central.heartbeatIntervalSeconds;
    if (!sConfig->central.enrollmentToken.isEmpty() && sConfig->central.enrollmentToken != previousEnrollmentToken) {
      sConfig->central.deviceId = "";
      sConfig->central.deviceToken = "";
    }

    sCfgMgr->save(*sConfig);
    sMonitor->resetIncident();
    sEventLog->add("config", "Configuration saved by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });


  server.on("/api/system/reboot", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Reboot requested by API");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(100);
    ESP.restart();
  });

  server.on("/api/system/factory-reset", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Factory reset requested by API");
    sCfgMgr->reset();
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(100);
    ESP.restart();
  });

  server.on("/api/system/ota", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (sOta->hasError()) {
      String body = "{\"ok\":false,\"error\":\"" + sOta->errorString() + "\"}";
      server.send(500, "application/json", body);
      return;
    }
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(250);
    ESP.restart();
  }, []() {
    if (sAuth && !sAuth->isAuthorized(server)) return;
    HTTPUpload& upload = server.upload();
    sOta->handleUpload(upload);
  });

  server.on("/style.css", HTTP_GET, []() {
    serveFileOrFallback("/style.css", "text/css", FALLBACK_STYLE_CSS);
  });

  server.on("/app.js", HTTP_GET, []() {
    serveFileOrFallback("/app.js", "application/javascript", FALLBACK_APP_JS);
  });

  server.on("/", HTTP_GET, []() {
    serveFileOrFallback("/index.html", "text/html", FALLBACK_INDEX_HTML);
  });

  server.begin();
}

void WebServerManager::loop() {
  server.handleClient();
}
