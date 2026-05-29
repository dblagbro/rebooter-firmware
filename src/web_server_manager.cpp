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
#include "firmware_version.h"
#include "monitor_engine.h"
#include "ota_manager.h"
#include "auth_manager.h"
#include "status_payload.h"
#include "wifi_manager.h"
#include "crash_recorder.h"
#include "power_monitor.h"
#include "discovery_manager.h"

static const char FALLBACK_INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Rebooter</title>
  <link rel="icon" href="data:,">
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

    <section class="band">
      <article class="panel auth-panel">
        <div class="auth-header">
          <div>
            <h2>Local Access</h2>
            <p class="hint" id="auth-note">Checking whether local auth is required...</p>
          </div>
          <div class="auth-state" id="auth-state">Checking</div>
        </div>
        <form id="auth-form" class="auth-form">
          <label>
            <span>Password</span>
            <input id="auth-password" type="password" maxlength="64" autocomplete="current-password" placeholder="Enter local admin password">
          </label>
          <div class="actions">
            <button id="auth-submit" type="submit">Unlock Protected Actions</button>
            <button id="auth-clear" type="button" class="secondary">Clear</button>
          </div>
        </form>
      </article>
    </section>

    <section class="band grid-two">
      <article class="panel">
        <h2>Device</h2>
        <dl class="kv">
          <div><dt>Name</dt><dd id="device-name">-</dd></div>
          <div><dt>Firmware</dt><dd id="firmware-version">-</dd></div>
          <div><dt>Mode</dt><dd id="device-mode">-</dd></div>
          <div><dt>Relay</dt><dd id="relay-state">-</dd></div>
          <div><dt>Wi-Fi</dt><dd id="wifi-state">-</dd></div>
          <div><dt>IP</dt><dd id="device-ip">-</dd></div>
          <div><dt>Setup AP</dt><dd id="setup-ap">-</dd></div>
          <div><dt>Uptime</dt><dd id="uptime">-</dd></div>
        </dl>
      </article>

      <article class="panel">
        <h2>Relay</h2>
        <div class="actions">
          <button id="relay-on" data-protected="true">Turn On</button>
          <button id="relay-off" data-protected="true">Turn Off</button>
          <button id="relay-toggle" data-protected="true">Toggle</button>
          <button id="refresh-status" class="secondary">Refresh</button>
        </div>
        <p class="hint" id="relay-hint">Relay commands require auth only after you set an admin password.</p>
      </article>
    </section>

    <section class="band grid-two">
      <article class="panel">
        <h2>Provisioning</h2>
        <p class="hint">This firmware first tries saved Wi-Fi plus the built-in default networks. If none work, the full device falls back to its setup access point so you can join it from a phone or laptop, browse to 192.168.4.1, and enter the real WLAN credentials. The setup network is open by default.</p>
        <form id="config-form" class="stack">
          <label>
            <span class="label-row"><span>Device Name</span><details class="field-help"><summary aria-label="Help for Device Name">?</summary><div class="help-card">This is the friendly name shown in the local UI and any central management views. It does not change device behavior, so pick something short and recognizable like "Office Modem" or "Lab Router Rebooter."</div></details></span>
            <input id="cfg-device-name" name="device_name" maxlength="32">
          </label>

          <label>
            <span class="label-row"><span>Mode</span><details class="field-help"><summary aria-label="Help for Mode">?</summary><div class="help-card">This chooses how the plug behaves. Smart Plug is manual control, Internet Watchdog checks outside connectivity, and Device Watchdog watches one specific host or IP for failure.</div></details></span>
            <select id="cfg-mode" name="current_mode">
              <option value="smart_plug">Smart Plug</option>
              <option value="internet_watchdog">Internet Watchdog</option>
              <option value="device_watchdog">Device Watchdog</option>
            </select>
          </label>

          <label>
            <span class="label-row"><span>Relay Restore</span><details class="field-help"><summary aria-label="Help for Relay Restore">?</summary><div class="help-card">This decides what the relay does after a reboot or power interruption. "Restore Previous" is usually the safest choice for inline equipment because it tries to return to the last known state.</div></details></span>
            <select id="cfg-restore" name="relay_restore_behavior">
              <option value="restore_previous">Restore Previous</option>
              <option value="always_on">Always On</option>
              <option value="always_off">Always Off</option>
            </select>
          </label>

          <label>
            <span class="label-row"><span>Monitor Interval (sec)</span><details class="field-help"><summary aria-label="Help for Monitor Interval">?</summary><div class="help-card">This is how often the watchdog logic wakes up to evaluate the current mode. Lower values react faster, but they also create more network traffic and more frequent checks.</div></details></span>
            <input id="cfg-monitor-interval" name="monitor_interval_seconds" type="number" min="2" max="3600">
          </label>

          <label>
            <span class="label-row"><span>Boot Warm-up (sec)</span><details class="field-help"><summary aria-label="Help for Boot Warm-up">?</summary><div class="help-card">This is the grace period after boot before watchdog actions are allowed to trigger. Use it to give routers, modems, or attached devices enough time to come up cleanly before the plug judges them unhealthy.</div></details></span>
            <input id="cfg-boot-warmup" name="boot_warmup_seconds" type="number" min="0" max="600">
          </label>

          <label class="checkbox-row">
            <input id="cfg-manual-button" name="manual_button_enabled" type="checkbox">
            <span class="label-row"><span>Enable short button press for relay in Smart Plug mode</span><details class="field-help"><summary aria-label="Help for Manual Button">?</summary><div class="help-card">When this is enabled, a short press on the physical button toggles the relay while in Smart Plug mode. Turn it off if you want to prevent accidental manual power changes on important equipment.</div></details></span>
          </label>

          <label class="checkbox-row">
            <input id="cfg-status-led" name="status_led_enabled" type="checkbox">
            <span>Enable the status LED</span>
          </label>

          <label>
            <span>Timezone</span>
            <input id="cfg-timezone" name="timezone" maxlength="64" placeholder="America/New_York">
          </label>

          <label>
            <span>Event Log Max Entries</span>
            <input id="cfg-event-log-max" name="event_log_max_entries" type="number" min="25" max="1000">
          </label>

          <label>
            <span>Notification Cooldown (sec)</span>
            <input id="cfg-notification-cooldown" name="notification_cooldown_seconds" type="number" min="0" max="3600">
          </label>

          <section class="mode-section">
            <h3>Notifications</h3>
            <label class="checkbox-row">
              <input id="cfg-notify-enabled" type="checkbox">
              <span>Enable outbound notifications</span>
            </label>
            <label><span>Webhook URL</span><input id="cfg-notify-webhook-url" maxlength="256" placeholder="https://example.com/hook"></label>
            <label><span>Webhook Auth Token</span><input id="cfg-notify-webhook-token" type="password" maxlength="128" placeholder="Leave blank to keep current"></label>
            <label class="checkbox-row"><input id="cfg-notify-on-trigger" type="checkbox"><span>Notify when a watchdog incident triggers</span></label>
            <label class="checkbox-row"><input id="cfg-notify-on-recovery" type="checkbox"><span>Notify when an incident recovers</span></label>
            <label class="checkbox-row"><input id="cfg-notify-on-max-cycles" type="checkbox"><span>Notify when the cycle limit is reached</span></label>
            <label class="checkbox-row"><input id="cfg-notify-test-enabled" type="checkbox"><span>Allow sending test notifications</span></label>
          </section>

          <section id="cfg-internet-section" class="mode-section">
            <h3>Internet Watchdog</h3>
            <label>
              <span class="label-row"><span>Targets (one host or IP per line)</span><details class="field-help"><summary aria-label="Help for Internet Targets">?</summary><div class="help-card">These are the external hosts or IPs the plug will check to decide whether internet access is healthy. Use a few reliable targets so one flaky service does not trigger an unnecessary power cycle.</div></details></span>
              <textarea id="cfg-internet-targets" rows="4" placeholder="1.1.1.1&#10;8.8.8.8&#10;time.nist.gov"></textarea>
            </label>
            <label><span class="label-row"><span>Failure Threshold (sec)</span><details class="field-help"><summary aria-label="Help for Internet Failure Threshold">?</summary><div class="help-card">This is how long the internet checks must fail before the plug treats the outage as real. Longer thresholds avoid reacting to brief hiccups, while shorter thresholds respond faster to true outages.</div></details></span><input id="cfg-internet-failure-threshold" type="number" min="10" max="86400"></label>
            <label><span class="label-row"><span>Power Off Duration (sec)</span><details class="field-help"><summary aria-label="Help for Internet Power Off Duration">?</summary><div class="help-card">This controls how long the relay stays off during a recovery cycle. It should be long enough for the attached modem or router to truly lose power and restart cleanly.</div></details></span><input id="cfg-internet-power-off" type="number" min="1" max="300"></label>
            <label><span class="label-row"><span>Post-Reboot Holdoff (sec)</span><details class="field-help"><summary aria-label="Help for Internet Post-Reboot Holdoff">?</summary><div class="help-card">This is the quiet period after power is restored before new failures count again. It prevents the device from immediately cycling again while the attached equipment is still booting.</div></details></span><input id="cfg-internet-post-reboot-holdoff" type="number" min="10" max="86400"></label>
            <label><span class="label-row"><span>Max Cycles Per Incident</span><details class="field-help"><summary aria-label="Help for Max Cycles Per Incident">?</summary><div class="help-card">This caps how many recovery attempts are allowed for one continuous outage event. It stops a single bad incident from causing endless power flapping.</div></details></span><input id="cfg-internet-max-cycles-incident" type="number" min="1" max="20"></label>
            <label><span class="label-row"><span>Max Cycles Per Hour</span><details class="field-help"><summary aria-label="Help for Max Cycles Per Hour">?</summary><div class="help-card">This is the broader rate limit across all incidents in a rolling hour. It protects equipment and power stability if something upstream is behaving badly.</div></details></span><input id="cfg-internet-max-cycles-hour" type="number" min="1" max="60"></label>
            <label><span class="label-row"><span>Cooldown (sec)</span><details class="field-help"><summary aria-label="Help for Cooldown">?</summary><div class="help-card">After the device hits its retry limits, this cooldown pauses further automatic action for a while. It gives the network time to settle and keeps the plug from thrashing.</div></details></span><input id="cfg-internet-cooldown" type="number" min="60" max="86400"></label>
            <label><span class="label-row"><span>DNS Refresh (sec)</span><details class="field-help"><summary aria-label="Help for DNS Refresh">?</summary><div class="help-card">This controls how often the device refreshes DNS-backed targets instead of reusing old lookups forever. It matters most when you watch hostnames instead of fixed IP addresses.</div></details></span><input id="cfg-internet-dns-refresh" type="number" min="60" max="86400"></label>
            <label><span class="label-row"><span>Recovery Stability (sec)</span><details class="field-help"><summary aria-label="Help for Recovery Stability">?</summary><div class="help-card">This requires the checks to stay healthy for a minimum time before the incident is considered fully recovered. It helps avoid bouncing between failed and recovered states during marginal connectivity.</div></details></span><input id="cfg-internet-recovery-stability" type="number" min="0" max="3600"></label>
          </section>

          <section id="cfg-device-section" class="mode-section">
            <h3>Device Watchdog</h3>
            <label><span class="label-row"><span>Target Host or IP</span><details class="field-help"><summary aria-label="Help for Device Target">?</summary><div class="help-card">This is the specific device or service the plug should watch, usually by IP or stable hostname. Pick the thing whose failure should justify power-cycling the outlet.</div></details></span><input id="cfg-device-target" maxlength="128" placeholder="192.168.1.1"></label>
            <label><span class="label-row"><span>Failure Threshold (sec)</span><details class="field-help"><summary aria-label="Help for Device Failure Threshold">?</summary><div class="help-card">This is how long the watched device must appear down before the plug reacts. It filters out short blips so the outlet only acts on sustained problems.</div></details></span><input id="cfg-device-failure-threshold" type="number" min="10" max="86400"></label>
            <label><span class="label-row"><span>Power Off Duration (sec)</span><details class="field-help"><summary aria-label="Help for Device Power Off Duration">?</summary><div class="help-card">This is how long the outlet cuts power during a recovery attempt. Set it long enough that the attached hardware truly shuts down before power comes back.</div></details></span><input id="cfg-device-power-off" type="number" min="1" max="300"></label>
            <label><span class="label-row"><span>Post-Reboot Holdoff (sec)</span><details class="field-help"><summary aria-label="Help for Device Post-Reboot Holdoff">?</summary><div class="help-card">This quiet period starts after power is restored and blocks immediate retriggering. It gives the attached device time to boot and rejoin the network normally.</div></details></span><input id="cfg-device-post-reboot-holdoff" type="number" min="10" max="86400"></label>
            <label><span class="label-row"><span>Max Cycles Per Incident</span><details class="field-help"><summary aria-label="Help for Device Max Cycles Per Incident">?</summary><div class="help-card">This limits how many times one failure episode can trigger a reboot attempt. It prevents endless cycling when the watched device has a deeper problem.</div></details></span><input id="cfg-device-max-cycles-incident" type="number" min="1" max="20"></label>
            <label><span class="label-row"><span>Max Cycles Per Hour</span><details class="field-help"><summary aria-label="Help for Device Max Cycles Per Hour">?</summary><div class="help-card">This rate limit applies across repeated incidents in an hour. It is a safety cap to avoid hammering the outlet or the attached hardware.</div></details></span><input id="cfg-device-max-cycles-hour" type="number" min="1" max="60"></label>
            <label><span class="label-row"><span>Cooldown (sec)</span><details class="field-help"><summary aria-label="Help for Device Cooldown">?</summary><div class="help-card">Once retry limits are reached, cooldown pauses further automatic action for this many seconds. That pause gives you breathing room to inspect the device instead of letting it loop forever.</div></details></span><input id="cfg-device-cooldown" type="number" min="60" max="86400"></label>
            <label><span class="label-row"><span>Recovery Stability (sec)</span><details class="field-help"><summary aria-label="Help for Device Recovery Stability">?</summary><div class="help-card">This requires the watched device to stay healthy for a minimum time before the incident is considered resolved. It helps avoid false recovery when a device flaps briefly back online.</div></details></span><input id="cfg-device-recovery-stability" type="number" min="0" max="3600"></label>
          </section>

          <section class="mode-section">
            <h3>Wi-Fi Networks</h3>
            <p class="hint">Saved networks are tried in priority order on boot, then built-in defaults, then the setup AP. Passwords are write-only; blank keeps the stored one.</p>
            <div id="cfg-wifi-networks" class="repeat-rows"></div>
            <div class="actions">
              <button id="cfg-wifi-add" type="button" class="secondary">Add network</button>
              <button id="cfg-wifi-scan" type="button" class="secondary" data-protected="true">Scan</button>
            </div>
            <p class="hint" id="cfg-wifi-scan-result"></p>
            <label>
              <span>Connect Timeout per network (ms)</span>
              <input id="cfg-wifi-timeout" name="wifi_connect_timeout_ms" type="number" min="5000" max="60000">
            </label>
          </section>

          <section class="mode-section">
            <h3>Central Service</h3>
            <label class="checkbox-row">
              <input id="cfg-central-enabled" type="checkbox">
              <span>Enable central management</span>
            </label>
            <label><span>Hub URLs (up to 10)</span></label>
            <div id="cfg-hub-urls" class="repeat-rows"></div>
            <div class="actions">
              <button id="cfg-hub-url-add" type="button" class="secondary">Add another hub URL</button>
            </div>
          </section>

          <label>
            <span class="label-row"><span>Admin Username</span><details class="field-help"><summary aria-label="Help for Admin Username">?</summary><div class="help-card">This is the local username used to protect the device web UI and API after you set credentials. Keep it memorable, because this login is separate from any future central account.</div></details></span>
            <input id="cfg-admin-username" name="admin_username" maxlength="32" placeholder="admin">
          </label>

          <label>
            <span class="label-row"><span>Admin Password</span><details class="field-help"><summary aria-label="Help for Admin Password">?</summary><div class="help-card">This password protects the local device interface and OTA endpoint. Leave it blank when saving if you want to keep the current password unchanged.</div></details></span>
            <input id="cfg-admin-password" name="admin_password" type="password" minlength="8" maxlength="64" placeholder="Leave blank to keep current">
          </label>

          <div class="actions">
            <button id="config-save" type="submit" data-protected="true">Save Settings</button>
          </div>
        </form>
      </article>

      <article class="panel">
        <h2>Firmware Update</h2>
        <form id="ota-form" class="stack">
          <label>
            <span class="label-row"><span>Firmware Bin</span><details class="field-help"><summary aria-label="Help for Firmware Bin">?</summary><div class="help-card">Choose a compiled `.bin` firmware file to update this device over the network. The upload only affects this one unit, and the device will reboot automatically after the OTA completes.</div></details></span>
            <input id="ota-file" type="file" accept=".bin,application/octet-stream">
          </label>
          <div class="actions">
            <button id="ota-submit" type="submit" data-protected="true">Upload Firmware</button>
          </div>
        </form>
        <div class="progress-wrap">
          <progress id="ota-progress" max="100" value="0"></progress>
          <span id="ota-progress-label">Idle</span>
        </div>
        <p class="hint">This uses the device's local OTA endpoint. No serial cable needed after bootstrap.</p>
      </article>
    </section>

    <section class="band">
      <article class="panel">
        <h2>Events</h2>
        <pre id="events-view">Loading events...</pre>
      </article>
    </section>

    <section class="band">
      <article class="panel">
        <h2>Message Log</h2>
        <pre id="message-log">Ready.</pre>
      </article>
    </section>
  </main>
  <script src="/app.js"></script>
</body>
</html>
)HTML";

static const char FALLBACK_STYLE_CSS[] PROGMEM = R"CSS(
* {
  box-sizing: border-box;
}

body {
  margin: 0;
  font-family: Arial, sans-serif;
  background: #eef2f0;
  color: #17211d;
}

.shell {
  max-width: 1080px;
  margin: 0 auto;
  padding: 20px;
}

.topbar {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 16px;
  padding: 20px 0 8px;
}

.topbar h1 {
  margin: 0 0 8px;
  font-size: 34px;
}

.topbar p {
  margin: 0;
  color: #54625c;
}

.band {
  margin-top: 18px;
}

.grid-two {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 18px;
}

.panel {
  background: #ffffff;
  border: 1px solid #cfd8d4;
  border-radius: 8px;
  padding: 18px;
}

.panel h2 {
  margin: 0 0 14px;
  font-size: 20px;
}

.auth-panel {
  display: grid;
  gap: 14px;
}

.auth-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 12px;
}

.auth-form {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 12px;
  align-items: end;
}

.auth-state {
  min-width: 124px;
  text-align: center;
  padding: 10px 12px;
  border-radius: 8px;
  font-weight: 700;
  background: #d8e0dc;
  color: #21312b;
}

.auth-state.unlocked {
  background: #d8f0da;
  color: #1e5c27;
}

.auth-state.locked {
  background: #f3e4b9;
  color: #6d5614;
}

.auth-state.unneeded {
  background: #dbe8f8;
  color: #21446b;
}

.kv {
  margin: 0;
}

.kv div {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  padding: 8px 0;
  border-bottom: 1px solid #edf2ef;
}

.kv div:last-child {
  border-bottom: 0;
}

.kv dt {
  font-weight: 700;
}

.kv dd {
  margin: 0;
  text-align: right;
  color: #42514b;
}

.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
}

button,
input,
select,
textarea {
  font: inherit;
}

button {
  appearance: none;
  border: 1px solid #124d39;
  background: #124d39;
  color: #ffffff;
  border-radius: 8px;
  padding: 10px 14px;
  cursor: pointer;
}

button.secondary {
  background: #ffffff;
  color: #124d39;
}

button:disabled {
  opacity: 0.55;
  cursor: not-allowed;
}

.stack {
  display: grid;
  gap: 12px;
}

label {
  display: grid;
  gap: 6px;
}

label span {
  font-size: 14px;
  color: #42514b;
}

.label-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
}

.field-help {
  position: relative;
}

.field-help summary {
  list-style: none;
  width: 20px;
  height: 20px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: 999px;
  border: 1px solid #8ba39a;
  background: #f4f8f6;
  color: #124d39;
  font-size: 12px;
  font-weight: 700;
  cursor: pointer;
  user-select: none;
}

.field-help summary::-webkit-details-marker {
  display: none;
}

.field-help[open] summary {
  background: #124d39;
  color: #ffffff;
  border-color: #124d39;
}

.help-card {
  position: absolute;
  right: 0;
  top: calc(100% + 8px);
  width: min(300px, 72vw);
  padding: 12px;
  border: 1px solid #cfd8d4;
  border-radius: 8px;
  background: #ffffff;
  box-shadow: 0 10px 30px rgba(23, 33, 29, 0.12);
  color: #42514b;
  font-size: 13px;
  line-height: 1.45;
  z-index: 20;
}

input,
select,
textarea {
  width: 100%;
  border: 1px solid #b8c5c0;
  border-radius: 8px;
  padding: 10px 12px;
  background: #ffffff;
  color: #17211d;
}

textarea {
  resize: vertical;
  min-height: 96px;
}

.checkbox-row {
  grid-template-columns: auto 1fr;
  align-items: center;
  column-gap: 10px;
}

.checkbox-row input {
  width: auto;
}

.mode-section {
  display: grid;
  gap: 12px;
  padding: 14px;
  border: 1px solid #dee7e2;
  border-radius: 8px;
  background: #f8fbf9;
}

.mode-section h3 {
  margin: 0;
  font-size: 16px;
}

.mode-section.hidden {
  display: none;
}

.status-pill {
  min-width: 116px;
  text-align: center;
  padding: 10px 12px;
  border-radius: 8px;
  font-weight: 700;
  background: #d8e0dc;
  color: #21312b;
}

.status-pill.healthy {
  background: #d8f0da;
  color: #1e5c27;
}

.status-pill.partial-failure,
.status-pill.holdoff {
  background: #f3e4b9;
  color: #6d5614;
}

.status-pill.failed,
.status-pill.cooldown {
  background: #f3c7bf;
  color: #7b2319;
}

.progress-wrap {
  display: grid;
  gap: 8px;
  margin-top: 14px;
}

progress {
  width: 100%;
  height: 16px;
}

.hint {
  margin: 10px 0 0;
  color: #5d6c66;
  font-size: 14px;
}

pre {
  margin: 0;
  padding: 14px;
  background: #f6f9f7;
  border: 1px solid #dee7e2;
  border-radius: 8px;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

@media (max-width: 800px) {
  .grid-two {
    grid-template-columns: 1fr;
  }

  .topbar {
    flex-direction: column;
  }

  .topbar h1 {
    font-size: 28px;
  }

  .auth-form {
    grid-template-columns: 1fr;
  }

  .help-card {
    left: 0;
    right: auto;
    width: min(320px, calc(100vw - 64px));
  }
}

.repeat-rows {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.hub-url-row {
  display: flex;
  gap: 8px;
  align-items: center;
}

.hub-url-row .hub-url-input {
  flex: 1;
}

.wifi-row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
}

.wifi-row .wifi-ssid,
.wifi-row .wifi-pass {
  flex: 1 1 140px;
}
)CSS";

static const char FALLBACK_APP_JS[] PROGMEM = R"JS(
const state = {
  status: null,
  config: null,
  authToken: sessionStorage.getItem('rebooterAuth') || '',
};

const $ = (id) => document.getElementById(id);

function logMessage(message) {
  const view = $('message-log');
  const stamp = new Date().toLocaleTimeString();
  view.textContent = `[${stamp}] ${message}\n` + view.textContent;
}

function formatUptime(seconds) {
  const s = Number(seconds || 0);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const rem = s % 60;
  return `${d}d ${h}h ${m}m ${rem}s`;
}

function setHealthPill(health) {
  const pill = $('health-pill');
  pill.textContent = health || 'unknown';
  pill.className = `status-pill ${String(health || 'unknown').replace(/_/g, '-')}`;
}

function authRequired() {
  return !!state.status?.auth_required;
}

function hasAuthToken() {
  return !!state.authToken;
}

function protectedActionsUnlocked() {
  return !authRequired() || hasAuthToken();
}

function updateProtectedControls() {
  document.querySelectorAll('[data-protected="true"]').forEach((element) => {
    element.disabled = !protectedActionsUnlocked();
  });
}

function renderAuth() {
  const note = $('auth-note');
  const stateBadge = $('auth-state');
  const password = $('auth-password');

  if (!state.status) {
    note.textContent = 'Checking whether local auth is required...';
    stateBadge.textContent = 'Checking';
    stateBadge.className = 'auth-state';
    updateProtectedControls();
    return;
  }

  if (!authRequired()) {
    note.textContent = 'This device currently allows local protected actions without an admin password.';
    stateBadge.textContent = 'Open';
    stateBadge.className = 'auth-state unneeded';
    password.value = '';
  } else if (hasAuthToken()) {
    note.textContent = 'Protected actions are unlocked for this browser tab. Clear the session when you are done.';
    stateBadge.textContent = 'Unlocked';
    stateBadge.className = 'auth-state unlocked';
  } else {
    note.textContent = 'This device requires the local admin password before relay, save, OTA, and system actions are allowed.';
    stateBadge.textContent = 'Locked';
    stateBadge.className = 'auth-state locked';
  }

  updateProtectedControls();
}

function splitTargets(value) {
  return String(value || '')
    .split(/\r?\n/)
    .map((entry) => entry.trim())
    .filter(Boolean);
}

const MAX_WIFI_NETWORKS = 5;

function makeWifiRow(network) {
  const row = document.createElement('div');
  row.className = 'wifi-row';

  const ssid = document.createElement('input');
  ssid.type = 'text';
  ssid.className = 'wifi-ssid';
  ssid.maxLength = 32;
  ssid.placeholder = 'SSID';
  ssid.value = network.ssid || '';

  const pass = document.createElement('input');
  pass.type = 'password';
  pass.className = 'wifi-pass';
  pass.maxLength = 64;
  pass.placeholder = network.has_password ? 'Saved (blank = keep)' : 'Password (blank = open)';
  pass.dataset.hasPassword = network.has_password ? '1' : '0';

  const up = document.createElement('button');
  up.type = 'button';
  up.className = 'secondary';
  up.textContent = 'Up';
  up.addEventListener('click', () => {
    const prev = row.previousElementSibling;
    if (prev) row.parentNode.insertBefore(row, prev);
  });

  const down = document.createElement('button');
  down.type = 'button';
  down.className = 'secondary';
  down.textContent = 'Down';
  down.addEventListener('click', () => {
    const next = row.nextElementSibling;
    if (next) row.parentNode.insertBefore(next, row);
  });

  const remove = document.createElement('button');
  remove.type = 'button';
  remove.className = 'secondary';
  remove.textContent = 'Remove';
  remove.addEventListener('click', () => row.remove());

  row.appendChild(ssid);
  row.appendChild(pass);
  row.appendChild(up);
  row.appendChild(down);
  row.appendChild(remove);
  return row;
}

function addWifiRow(network) {
  const container = $('cfg-wifi-networks');
  if (container.querySelectorAll('.wifi-row').length >= MAX_WIFI_NETWORKS) return;
  container.appendChild(makeWifiRow(network || {}));
}

function renderWifiNetworks(networks) {
  const container = $('cfg-wifi-networks');
  container.innerHTML = '';
  (Array.isArray(networks) ? networks : []).slice(0, MAX_WIFI_NETWORKS)
    .forEach((network) => addWifiRow(network));
}

function collectWifiNetworks() {
  return Array.from(document.querySelectorAll('.wifi-row'))
    .map((row) => {
      const ssid = row.querySelector('.wifi-ssid').value.trim();
      const passInput = row.querySelector('.wifi-pass');
      const entry = { ssid };
      if (passInput.value) {
        entry.password = passInput.value;
      } else if (passInput.dataset.hasPassword !== '1') {
        entry.password = '';
      }
      return entry;
    })
    .filter((entry) => entry.ssid)
    .slice(0, MAX_WIFI_NETWORKS);
}

async function handleWifiScan() {
  if (!protectedActionsUnlocked()) {
    logMessage('Wi-Fi scan blocked: unlock protected actions first.');
    return;
  }
  const result = $('cfg-wifi-scan-result');
  result.textContent = 'Scanning...';
  try {
    const data = await postJson('/api/wifi/scan', {});
    const networks = Array.isArray(data.networks) ? data.networks : [];
    networks.sort((a, b) => (b.rssi || -999) - (a.rssi || -999));
    result.textContent = networks.length
      ? networks.map((n) => `${n.ssid} (${n.rssi} dBm${n.secure ? '' : ', open'})`).join('  |  ')
      : 'No networks found.';
  } catch (error) {
    result.textContent = `Scan failed: ${error.message}`;
  }
}

const MAX_HUB_URLS = 10;

function addHubUrlRow(value) {
  const container = $('cfg-hub-urls');
  if (container.querySelectorAll('.hub-url-row').length >= MAX_HUB_URLS) return;
  const row = document.createElement('div');
  row.className = 'hub-url-row';
  const input = document.createElement('input');
  input.type = 'text';
  input.className = 'hub-url-input';
  input.maxLength = 192;
  input.placeholder = 'https://hub.example.com/rebooter';
  input.value = value || '';
  const remove = document.createElement('button');
  remove.type = 'button';
  remove.className = 'secondary';
  remove.textContent = 'Remove';
  remove.addEventListener('click', () => {
    row.remove();
    if (!$('cfg-hub-urls').querySelectorAll('.hub-url-row').length) addHubUrlRow('');
  });
  row.appendChild(input);
  row.appendChild(remove);
  container.appendChild(row);
}

function renderHubUrls(urls) {
  const container = $('cfg-hub-urls');
  container.innerHTML = '';
  const list = Array.isArray(urls) && urls.length ? urls : [''];
  list.slice(0, MAX_HUB_URLS).forEach((url) => addHubUrlRow(url));
}

function collectHubUrls() {
  return Array.from(document.querySelectorAll('.hub-url-input'))
    .map((input) => input.value.trim())
    .filter(Boolean)
    .slice(0, MAX_HUB_URLS);
}

function renderModeSections() {
  const mode = $('cfg-mode').value;
  $('cfg-internet-section').classList.toggle('hidden', mode !== 'internet_watchdog');
  $('cfg-device-section').classList.toggle('hidden', mode !== 'device_watchdog');
}

function renderStatus() {
  if (!state.status) return;
  $('device-name').textContent = state.status.device_name || '-';
  $('firmware-version').textContent = state.status.firmware_version || '-';
  $('device-mode').textContent = state.status.mode || '-';
  $('relay-state').textContent = state.status.relay_on ? 'On' : 'Off';
  $('wifi-state').textContent = state.status.wifi_connected ? 'Connected' : 'Disconnected';
  $('device-ip').textContent = window.location.host || '-';
  $('setup-ap').textContent = state.status.setup_ap_name || '-';
  $('uptime').textContent = formatUptime(state.status.uptime_seconds);
  if (state.status.in_captive_portal) {
    $('connection-note').textContent = `Setup AP is active. Join ${state.status.setup_ap_name || 'the Rebooter setup network'} and browse to 192.168.4.1 to provision Wi-Fi. The setup network is open.`;
  } else {
    $('connection-note').textContent = state.status.wifi_connected
      ? 'Device is online and serving the local control plane.'
      : 'Device is up but Wi-Fi is not currently connected.';
  }
  setHealthPill(state.status.health_state);
  $('relay-hint').textContent = authRequired()
    ? 'Relay commands are locked until you unlock this browser tab with the local admin password.'
    : 'Relay commands are currently available without local auth.';
  renderAuth();
}

function renderConfig() {
  if (!state.config) return;
  $('cfg-device-name').value = state.config.device_name || '';
  $('cfg-mode').value = state.config.current_mode || 'smart_plug';
  $('cfg-restore').value = state.config.relay_restore_behavior || 'restore_previous';
  $('cfg-monitor-interval').value = state.config.monitor_interval_seconds ?? 5;
  $('cfg-boot-warmup').value = state.config.boot_warmup_seconds ?? 30;
  $('cfg-manual-button').checked = !!state.config.manual_button_enabled;
  $('cfg-status-led').checked = state.config.status_led_enabled !== false;
  $('cfg-timezone').value = state.config.timezone || 'America/New_York';
  $('cfg-event-log-max').value = state.config.event_log_max_entries ?? 200;
  $('cfg-notification-cooldown').value = state.config.notification_cooldown_seconds ?? 60;
  $('cfg-admin-username').value = state.config.admin_username || 'admin';

  const wifi = state.config.wifi || {};
  renderWifiNetworks(wifi.saved_networks);
  $('cfg-wifi-timeout').value = wifi.connect_timeout_ms ?? 15000;

  const central = state.config.central || {};
  $('cfg-central-enabled').checked = !!central.enabled;
  renderHubUrls(central.base_urls);

  const notifications = state.config.notifications || {};
  $('cfg-notify-enabled').checked = !!notifications.enabled;
  $('cfg-notify-webhook-url').value = notifications.webhook_url || '';
  $('cfg-notify-webhook-token').value = '';
  $('cfg-notify-on-trigger').checked = notifications.send_on_trigger !== false;
  $('cfg-notify-on-recovery').checked = notifications.send_on_recovery !== false;
  $('cfg-notify-on-max-cycles').checked = notifications.send_on_max_cycles_reached !== false;
  $('cfg-notify-test-enabled').checked = notifications.send_test_notification_enabled !== false;

  const internet = state.config.internet || {};
  $('cfg-internet-targets').value = Array.isArray(internet.targets) ? internet.targets.join('\n') : '';
  $('cfg-internet-failure-threshold').value = internet.failure_threshold_seconds ?? 180;
  $('cfg-internet-power-off').value = internet.power_off_seconds ?? 5;
  $('cfg-internet-post-reboot-holdoff').value = internet.post_reboot_holdoff_seconds ?? 180;
  $('cfg-internet-max-cycles-incident').value = internet.max_cycles_per_incident ?? 3;
  $('cfg-internet-max-cycles-hour').value = internet.max_cycles_per_hour ?? 6;
  $('cfg-internet-cooldown').value = internet.cooldown_seconds ?? 3600;
  $('cfg-internet-dns-refresh').value = internet.dns_refresh_seconds ?? 300;
  $('cfg-internet-recovery-stability').value = internet.recovery_stability_seconds ?? 15;

  const device = state.config.device || {};
  $('cfg-device-target').value = device.target || '';
  $('cfg-device-failure-threshold').value = device.failure_threshold_seconds ?? 60;
  $('cfg-device-power-off').value = device.power_off_seconds ?? 5;
  $('cfg-device-post-reboot-holdoff').value = device.post_reboot_holdoff_seconds ?? 300;
  $('cfg-device-max-cycles-incident').value = device.max_cycles_per_incident ?? 3;
  $('cfg-device-max-cycles-hour').value = device.max_cycles_per_hour ?? 6;
  $('cfg-device-cooldown').value = device.cooldown_seconds ?? 3600;
  $('cfg-device-recovery-stability').value = device.recovery_stability_seconds ?? 30;

  renderModeSections();
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function fetchJson(path, options = {}) {
  const { headers: optionHeaders, ...rest } = options;
  const headers = new Headers(optionHeaders || {});
  if (state.authToken) {
    headers.set('X-Rebooter-Auth', state.authToken);
  }
  const response = await fetch(path, {
    cache: 'no-store',
    ...rest,
    headers,
  });
  const text = await response.text();
  let json = {};
  if (text) {
    try {
      json = JSON.parse(text);
    } catch (_) {
      json = { raw: text };
    }
  }
  if (!response.ok) {
    if (response.status === 401 && state.authToken) {
      setAuthToken('');
    }
    throw new Error(json.error || `${response.status} ${response.statusText}`);
  }
  return json;
}

async function refreshStatus() {
  state.status = await fetchJson('/api/status');
  renderStatus();
}

async function refreshConfig() {
  state.config = await fetchJson('/api/config');
  renderConfig();
}

async function refreshEvents() {
  const events = await fetchJson('/api/events');
  $('events-view').textContent = JSON.stringify(events, null, 2);
}

async function refreshAll() {
  const results = await Promise.allSettled([refreshStatus(), refreshConfig(), refreshEvents()]);
  const failures = results
    .filter((result) => result.status === 'rejected')
    .map((result) => result.reason?.message || 'Unknown error');

  if (failures.length) {
    logMessage(`Refresh partially failed: ${failures.join('; ')}`);
  }
}

async function postJson(path, payload) {
  return fetchJson(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
}

function setAuthToken(token) {
  state.authToken = token || '';
  if (state.authToken) {
    sessionStorage.setItem('rebooterAuth', state.authToken);
  } else {
    sessionStorage.removeItem('rebooterAuth');
  }
  renderAuth();
}

async function handleAuthSubmit(event) {
  event.preventDefault();
  if (!authRequired()) {
    logMessage('Local auth is not required on this device right now.');
    return;
  }

  const password = $('auth-password').value.trim();
  if (!password) {
    logMessage('Enter the local admin password first.');
    return;
  }

  try {
    setAuthToken(password);
    await fetchJson('/api/system/heartbeat-preview');
    $('auth-password').value = '';
    logMessage('Protected actions unlocked for this browser tab.');
    await refreshAll();
  } catch (error) {
    setAuthToken('');
    logMessage(`Unlock failed: ${error.message}`);
  }
}

function handleAuthClear() {
  setAuthToken('');
  $('auth-password').value = '';
  logMessage('Cleared the local auth token for this browser tab.');
}

async function handleRelay(path, label) {
  if (!protectedActionsUnlocked()) {
    logMessage(`${label} blocked: unlock protected actions first.`);
    return;
  }
  const buttons = ['relay-on', 'relay-off', 'relay-toggle'].map($);
  try {
    buttons.forEach((button) => { button.disabled = true; });
    await fetchJson(path, { method: 'POST' });
    logMessage(`${label} command sent.`);
    await delay(250);
    await refreshStatus();
    setTimeout(() => {
      refreshStatus().catch((error) => logMessage(`Relay status follow-up failed: ${error.message}`));
    }, 1200);
  } catch (error) {
    logMessage(`${label} failed: ${error.message}`);
  } finally {
    buttons.forEach((button) => { button.disabled = false; });
  }
}

async function handleConfigSave(event) {
  event.preventDefault();
  if (!protectedActionsUnlocked()) {
    logMessage('Save blocked: unlock protected actions first.');
    return;
  }
  const payload = {
    device_name: $('cfg-device-name').value.trim(),
    current_mode: $('cfg-mode').value,
    relay_restore_behavior: $('cfg-restore').value,
    timezone: $('cfg-timezone').value.trim() || 'America/New_York',
    monitor_interval_seconds: Number($('cfg-monitor-interval').value || 5),
    boot_warmup_seconds: Number($('cfg-boot-warmup').value || 30),
    manual_button_enabled: $('cfg-manual-button').checked,
    status_led_enabled: $('cfg-status-led').checked,
    event_log_max_entries: Number($('cfg-event-log-max').value || 200),
    notification_cooldown_seconds: Number($('cfg-notification-cooldown').value || 60),
    admin_username: $('cfg-admin-username').value.trim(),
    internet: {
      targets: splitTargets($('cfg-internet-targets').value),
      failure_threshold_seconds: Number($('cfg-internet-failure-threshold').value || 180),
      power_off_seconds: Number($('cfg-internet-power-off').value || 5),
      post_reboot_holdoff_seconds: Number($('cfg-internet-post-reboot-holdoff').value || 180),
      max_cycles_per_incident: Number($('cfg-internet-max-cycles-incident').value || 3),
      max_cycles_per_hour: Number($('cfg-internet-max-cycles-hour').value || 6),
      cooldown_seconds: Number($('cfg-internet-cooldown').value || 3600),
      dns_refresh_seconds: Number($('cfg-internet-dns-refresh').value || 300),
      recovery_stability_seconds: Number($('cfg-internet-recovery-stability').value || 15),
    },
    device: {
      target: $('cfg-device-target').value.trim(),
      failure_threshold_seconds: Number($('cfg-device-failure-threshold').value || 60),
      power_off_seconds: Number($('cfg-device-power-off').value || 5),
      post_reboot_holdoff_seconds: Number($('cfg-device-post-reboot-holdoff').value || 300),
      max_cycles_per_incident: Number($('cfg-device-max-cycles-incident').value || 3),
      max_cycles_per_hour: Number($('cfg-device-max-cycles-hour').value || 6),
      cooldown_seconds: Number($('cfg-device-cooldown').value || 3600),
      recovery_stability_seconds: Number($('cfg-device-recovery-stability').value || 30),
    },
  };

  const password = $('cfg-admin-password').value;
  if (password) payload.admin_password = password;

  const notifications = {
    enabled: $('cfg-notify-enabled').checked,
    webhook_url: $('cfg-notify-webhook-url').value.trim(),
    send_on_trigger: $('cfg-notify-on-trigger').checked,
    send_on_recovery: $('cfg-notify-on-recovery').checked,
    send_on_max_cycles_reached: $('cfg-notify-on-max-cycles').checked,
    send_test_notification_enabled: $('cfg-notify-test-enabled').checked,
  };
  const notifyToken = $('cfg-notify-webhook-token').value;
  if (notifyToken) notifications.webhook_auth_token = notifyToken;
  payload.notifications = notifications;

  payload.wifi = {
    saved_networks: collectWifiNetworks(),
    connect_timeout_ms: Number($('cfg-wifi-timeout').value || 15000),
  };

  payload.central = {
    enabled: $('cfg-central-enabled').checked,
    base_urls: collectHubUrls(),
  };

  try {
    await postJson('/api/config/save', payload);
    if (password) {
      setAuthToken(password);
    }
    $('cfg-admin-password').value = '';
    logMessage('Settings saved.');
    await refreshAll();
  } catch (error) {
    logMessage(`Save failed: ${error.message}`);
  }
}

async function handleOtaSubmit(event) {
  event.preventDefault();
  if (!protectedActionsUnlocked()) {
    logMessage('OTA blocked: unlock protected actions first.');
    return;
  }
  const input = $('ota-file');
  const file = input.files && input.files[0];
  if (!file) {
    logMessage('Choose a firmware .bin first.');
    return;
  }

  const formData = new FormData();
  formData.append('update', file);

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/system/ota', true);
  if (state.authToken) {
    xhr.setRequestHeader('X-Rebooter-Auth', state.authToken);
  }

  xhr.upload.onprogress = (evt) => {
    if (!evt.lengthComputable) return;
    const percent = Math.round((evt.loaded / evt.total) * 100);
    $('ota-progress').value = percent;
    $('ota-progress-label').textContent = `${percent}%`;
  };

  xhr.onload = () => {
    if (xhr.status >= 200 && xhr.status < 300) {
      $('ota-progress').value = 100;
      $('ota-progress-label').textContent = 'Upload complete, device rebooting';
      logMessage('OTA upload accepted. Device is rebooting.');
    } else {
      $('ota-progress-label').textContent = `Failed (${xhr.status})`;
      logMessage(`OTA upload failed: ${xhr.responseText || xhr.status}`);
    }
  };

  xhr.onerror = () => {
    $('ota-progress-label').textContent = 'Network error';
    logMessage('OTA upload hit a network error.');
  };

  $('ota-progress').value = 0;
  $('ota-progress-label').textContent = 'Uploading...';
  logMessage(`Uploading firmware: ${file.name}`);
  xhr.send(formData);
}

function wireUi() {
  $('auth-form').addEventListener('submit', handleAuthSubmit);
  $('auth-clear').addEventListener('click', handleAuthClear);
  $('relay-on').addEventListener('click', () => handleRelay('/api/relay/on', 'Relay on'));
  $('relay-off').addEventListener('click', () => handleRelay('/api/relay/off', 'Relay off'));
  $('relay-toggle').addEventListener('click', () => handleRelay('/api/relay/toggle', 'Relay toggle'));
  $('refresh-status').addEventListener('click', refreshAll);
  $('cfg-mode').addEventListener('change', renderModeSections);
  $('cfg-hub-url-add').addEventListener('click', () => addHubUrlRow(''));
  $('cfg-wifi-add').addEventListener('click', () => addWifiRow({}));
  $('cfg-wifi-scan').addEventListener('click', handleWifiScan);
  $('config-form').addEventListener('submit', handleConfigSave);
  $('ota-form').addEventListener('submit', handleOtaSubmit);
}

wireUi();
renderAuth();
refreshAll();
setInterval(() => {
  refreshStatus().catch((error) => logMessage(`Background status refresh failed: ${error.message}`));
}, 5000);
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
static WifiManagerService* sWifi = nullptr;
static PowerMonitor* sPower = nullptr;
static DiscoveryManager* sDiscovery = nullptr;

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

  // 0.2.4: feed both watchdogs before the blocking TLS+GET. Same defensive
  // pattern as central_client.cpp's HTTPS sites — keeps the soft-WDT from
  // firing if a slow handshake takes >3.5s under heap pressure.
  ESP.wdtFeed();
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

static bool authRequiredForUi() {
  return sAuth && sAuth->isProvisioned();
}

static void sendMethodNotAllowed(const char* allowed) {
  server.sendHeader("Allow", allowed);
  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

static void sendConfigJson(bool includeProtectedFields = false) {
  JsonDocument doc;
  doc["schema_version"] = sConfig->schemaVersion;
  doc["device_name"] = sConfig->deviceName;
  doc["admin_username"] = sConfig->adminUsername;
  doc["current_mode"] = modeToString(sConfig->currentMode);
  doc["relay_restore_behavior"] = restoreToString(sConfig->relayRestoreBehavior);
  doc["last_relay_on"] = sConfig->lastRelayOn;
  doc["timezone"] = sConfig->timezone;
  doc["monitor_interval_seconds"] = sConfig->monitorIntervalSeconds;
  doc["boot_warmup_seconds"] = sConfig->bootWarmupSeconds;
  doc["manual_button_enabled"] = sConfig->manualButtonEnabled;
  doc["status_led_enabled"] = sConfig->statusLedEnabled;
  doc["event_log_max_entries"] = sConfig->eventLogMaxEntries;
  doc["notification_cooldown_seconds"] = sConfig->notificationCooldownSeconds;

  // Wi-Fi saved networks: never echo plaintext passwords on the read surface.
  JsonArray savedNetworks = doc["wifi"]["saved_networks"].to<JsonArray>();
  for (const auto& network : sConfig->wifi.savedNetworks) {
    JsonObject entry = savedNetworks.add<JsonObject>();
    entry["ssid"] = network.ssid;
    entry["has_password"] = !network.password.isEmpty();
  }
  doc["wifi"]["connect_timeout_ms"] = sConfig->wifi.connectTimeoutMs;
  doc["wifi"]["prefer_strongest_known"] = sConfig->wifi.preferStrongestKnown;
  doc["wifi"]["periodic_scan_enabled"] = sConfig->wifi.periodicScanEnabled;
  doc["wifi"]["periodic_scan_interval_seconds"] = sConfig->wifi.periodicScanIntervalSeconds;

  JsonArray targets = doc["internet"]["targets"].to<JsonArray>();
  for (const auto& target : sConfig->internet.targets) targets.add(target);
  doc["internet"]["failure_threshold_seconds"] = sConfig->internet.failureThresholdSeconds;
  doc["internet"]["power_off_seconds"] = sConfig->internet.powerOffSeconds;
  doc["internet"]["post_reboot_holdoff_seconds"] = sConfig->internet.postRebootHoldoffSeconds;
  doc["internet"]["max_cycles_per_incident"] = sConfig->internet.maxCyclesPerIncident;
  doc["internet"]["max_cycles_per_hour"] = sConfig->internet.maxCyclesPerHour;
  doc["internet"]["cooldown_seconds"] = sConfig->internet.cooldownSeconds;
  doc["internet"]["dns_refresh_seconds"] = sConfig->internet.dnsRefreshSeconds;
  doc["internet"]["recovery_stability_seconds"] = sConfig->internet.recoveryStabilitySeconds;

  doc["device"]["target"] = sConfig->device.target;
  doc["device"]["failure_threshold_seconds"] = sConfig->device.failureThresholdSeconds;
  doc["device"]["power_off_seconds"] = sConfig->device.powerOffSeconds;
  doc["device"]["post_reboot_holdoff_seconds"] = sConfig->device.postRebootHoldoffSeconds;
  doc["device"]["max_cycles_per_incident"] = sConfig->device.maxCyclesPerIncident;
  doc["device"]["max_cycles_per_hour"] = sConfig->device.maxCyclesPerHour;
  doc["device"]["cooldown_seconds"] = sConfig->device.cooldownSeconds;
  doc["device"]["recovery_stability_seconds"] = sConfig->device.recoveryStabilitySeconds;

  doc["notifications"]["enabled"] = sConfig->notifications.enabled;
  doc["notifications"]["type"] = sConfig->notifications.type;
  doc["notifications"]["webhook_url"] = sConfig->notifications.webhookUrl;
  doc["notifications"]["webhook_method"] = sConfig->notifications.webhookMethod;
  doc["notifications"]["has_webhook_auth_token"] = !sConfig->notifications.webhookAuthToken.isEmpty();
  doc["notifications"]["send_on_trigger"] = sConfig->notifications.sendOnTrigger;
  doc["notifications"]["send_on_recovery"] = sConfig->notifications.sendOnRecovery;
  doc["notifications"]["send_on_max_cycles_reached"] = sConfig->notifications.sendOnMaxCyclesReached;
  doc["notifications"]["send_test_notification_enabled"] = sConfig->notifications.sendTestNotificationEnabled;
  if (includeProtectedFields) {
    doc["notifications"]["webhook_auth_token"] = sConfig->notifications.webhookAuthToken;
  }

  doc["central"]["enabled"] = sConfig->central.enabled;
  JsonArray centralBaseUrls = doc["central"]["base_urls"].to<JsonArray>();
  for (const auto& url : sConfig->central.baseUrls) centralBaseUrls.add(url);
  doc["central"]["device_alias"] = sConfig->central.deviceAlias;
  doc["central"]["registered"] =
      !sConfig->central.deviceId.isEmpty() && !sConfig->central.deviceToken.isEmpty();
  doc["central"]["has_enrollment_token"] = !sConfig->central.enrollmentToken.isEmpty();
  if (includeProtectedFields) {
    doc["central"]["enrollment_token"] = sConfig->central.enrollmentToken;
    doc["central"]["site_id"] = sConfig->central.siteId;
    doc["central"]["device_id"] = sConfig->central.deviceId;
    doc["central"]["device_token"] = sConfig->central.deviceToken;
  }
  doc["central"]["poll_interval_seconds"] = sConfig->central.pollIntervalSeconds;
  doc["central"]["heartbeat_interval_seconds"] = sConfig->central.heartbeatIntervalSeconds;

  doc["power"]["enabled"] = sConfig->power.enabled;
  doc["power"]["sample_rate_hz"] = sConfig->power.sampleRateHz;
  doc["power"]["batch_seconds"] = sConfig->power.batchSeconds;
  doc["power"]["include_wifi_stats"] = sConfig->power.includeWifiStats;
  // include_frequency is intentionally not exposed: the CSE7766 path never produces a
  // mains-frequency value, so surfacing it as a settable field advertises a fake capability.

  doc["discovery"]["mdns_enabled"] = sConfig->discovery.mdnsEnabled;
  doc["discovery"]["udp_announce_enabled"] = sConfig->discovery.udpAnnounceEnabled;
  doc["discovery"]["udp_port"] = sConfig->discovery.udpPort;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void WebServerManager::begin(AppConfig* config, RuntimeStatus* status,
                             RelayController* relay, ConfigManager* cfgMgr,
                             EventLog* eventLog, MonitorEngine* monitor,
                             OtaManager* ota,
                             AuthManager* auth,
                             WifiManagerService* wifi,
                             PowerMonitor* power,
                             DiscoveryManager* discovery) {
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
  sWifi = wifi;
  sPower = power;
  sDiscovery = discovery;
  server.collectHeaders("X-Rebooter-Auth");

  server.on("/api/status", HTTP_GET, []() {
      JsonDocument doc;
      const uint32_t lastHeartbeatStamp = sStatus->centralLastHeartbeatSeconds;
      const uint32_t heartbeatAgeSeconds =
          (lastHeartbeatStamp > 0 && sStatus->uptimeSeconds >= lastHeartbeatStamp)
              ? (sStatus->uptimeSeconds - lastHeartbeatStamp)
              : 0;
      doc["device_name"] = sConfig->deviceName;
      doc["firmware_version"] = FIRMWARE_VERSION;
      doc["mode"] = modeToString(sConfig->currentMode);
      doc["relay_on"] = sRelay->isOn();
      doc["wifi_connected"] = sStatus->wifiConnected;
      // 0.2.7: current-connection RSSI (dBm), only when associated.
      if (WiFi.isConnected()) {
        doc["wifi_rssi_dbm"] = WiFi.RSSI();
      }
      // 0.2.8 (#154): latest opt-in periodic nearby-network scan (top-N),
      // for local verification + parity with the heartbeat. Present only
      // when the feature is on + a summary has been captured.
      {
        const String& scan = sWifi->latestScanSummary();
        if (scan.length() > 2) {
          JsonDocument scanDoc;
          if (deserializeJson(scanDoc, scan) == DeserializationError::Ok) {
            doc["wifi_scan"] = scanDoc;
            doc["wifi_scan_uptime_seconds"] = sWifi->latestScanUptimeSeconds();
          }
        }
      }
      doc["in_captive_portal"] = sStatus->inCaptivePortal;
      doc["recovery_mode"] = sStatus->recoveryMode;
      doc["auto_recovery_triggered"] = sStatus->autoRecoveryTriggered;
      doc["last_known_good_restored"] = sStatus->lastKnownGoodRestored;
      doc["previous_boot_different_firmware"] = sStatus->previousBootDifferentFirmware;
      doc["consecutive_unhealthy_boots"] = sStatus->consecutiveUnhealthyBoots;
      doc["setup_ap_name"] = sStatus->setupApName;
      doc["health_state"] = healthToString(sStatus->healthState);
      doc["uptime_seconds"] = sStatus->uptimeSeconds;
      doc["reset_reason"] = sStatus->resetReason;
      doc["last_planned_restart_reason"] = sStatus->lastPlannedRestartReason;
      doc["time_synced"] = sStatus->timeSynced;
      doc["wall_clock_unix_ms"] = sStatus->wallClockUnixMs;
      doc["free_heap"] = ESP.getFreeHeap();
      doc["incident_cycles"] = sStatus->currentIncidentCycles;
      doc["hour_cycles"] = sStatus->currentHourCycles;
      doc["holdoff_remaining_seconds"] = sStatus->holdoffRemainingSeconds;
      doc["cooldown_remaining_seconds"] = sStatus->cooldownRemainingSeconds;
      doc["auth_required"] = authRequiredForUi();
      doc["central_enabled"] = sStatus->centralEnabled;
      doc["central_registered"] = sStatus->centralRegistered;
      doc["central_state"] = sStatus->centralState;
      doc["central_identity_present"] = !sStatus->centralDeviceId.isEmpty();
      doc["central_last_heartbeat_seconds"] = lastHeartbeatStamp;
      doc["central_last_heartbeat_uptime_seconds"] = lastHeartbeatStamp;
      doc["central_heartbeat_age_seconds"] = heartbeatAgeSeconds;
      doc["last_crash_present"] = sStatus->lastCrashPresent;
      if (sStatus->lastCrashPresent) {
        doc["last_crash_reason"] = sStatus->lastCrashReason;
      }
      StatusPayload::fillPowerStatus(doc, *sConfig, sStatus);
      String out;
      serializeJson(doc, out);
      server.send(200, "application/json", out);
    });

  server.on("/api/config", HTTP_GET, []() {
    sendConfigJson();
  });

  server.on("/api/system/config-backup", HTTP_GET, []() {
    if (!requireAuth()) return;
    if (!sAuth->isProvisioned()) {
      server.send(409, "application/json", "{\"error\":\"set admin password before exporting protected backup\"}");
      return;
    }
    sendConfigJson(true);
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

  server.on("/api/system/heartbeat-preview", HTTP_GET, []() {
    if (!requireAuth()) return;

    JsonDocument doc;
    StatusPayload::fillHeartbeatDocument(doc, *sConfig, sStatus, FIRMWARE_VERSION, true);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Crash dumps reveal addresses and stack words, so keep them behind auth
  // like the other diagnostic endpoints.
  server.on("/api/system/crash", HTTP_GET, []() {
    if (!requireAuth()) return;
    server.send(200, "application/json", CrashRecorder::storedCrashesJson());
  });

  server.on("/api/system/crash/clear", HTTP_POST, []() {
    if (!requireAuth()) return;
    CrashRecorder::clearStoredCrashes();
    sStatus->lastCrashPresent = false;
    sStatus->lastCrashReason = "";
    sEventLog->add("crash", "Stored crash dumps cleared by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/system/crash/clear", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  // Fixed-size in-RAM ring of the most recent raw power samples. Public read,
  // consistent with the other read endpoints.
  server.on("/api/power/recent", HTTP_GET, []() {
    if (!sPower) {
      server.send(200, "application/json", "[]");
      return;
    }
    server.send(200, "application/json", sPower->recentSamplesJson());
  });

  server.on("/api/power/energy/reset", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (sPower) sPower->resetEnergy();
    sEventLog->add("power", "Energy accumulator reset by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/power/energy/reset", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  // On-demand LAN discovery UDP announce burst.
  server.on("/api/discovery/announce", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!sConfig->discovery.udpAnnounceEnabled) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"error\":\"udp announce disabled in config\"}");
      return;
    }
    if (sDiscovery) sDiscovery->triggerAnnounceBurst();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/discovery/announce", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

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
    sConfig->timezone = doc["timezone"] | sConfig->timezone;
    sConfig->monitorIntervalSeconds = doc["monitor_interval_seconds"] | sConfig->monitorIntervalSeconds;
    sConfig->bootWarmupSeconds = doc["boot_warmup_seconds"] | sConfig->bootWarmupSeconds;
    sConfig->manualButtonEnabled = doc["manual_button_enabled"] | sConfig->manualButtonEnabled;
    sConfig->statusLedEnabled = doc["status_led_enabled"] | sConfig->statusLedEnabled;
    sConfig->eventLogMaxEntries = doc["event_log_max_entries"] | sConfig->eventLogMaxEntries;
    sConfig->notificationCooldownSeconds = doc["notification_cooldown_seconds"] | sConfig->notificationCooldownSeconds;

    if (doc["wifi"].is<JsonObject>()) {
      if (doc["wifi"]["saved_networks"].is<JsonArray>()) {
        // Snapshot existing passwords so an edit-without-retype (password field
        // absent for a known SSID) keeps the stored password. A present-but-empty
        // password explicitly sets an open network.
        std::vector<WifiNetwork> previous = sConfig->wifi.savedNetworks;
        std::vector<WifiNetwork> updated;
        for (JsonVariant v : doc["wifi"]["saved_networks"].as<JsonArray>()) {
          WifiNetwork network;
          network.ssid = String((const char*)(v["ssid"] | ""));
          if (v["password"].is<const char*>()) {
            network.password = String((const char*)v["password"]);
          } else {
            network.password = "";
            for (const auto& old : previous) {
              if (old.ssid == network.ssid) { network.password = old.password; break; }
            }
          }
          updated.push_back(network);
        }
        sConfig->wifi.savedNetworks = updated;
      }
      sConfig->wifi.connectTimeoutMs = doc["wifi"]["connect_timeout_ms"] | sConfig->wifi.connectTimeoutMs;
      sConfig->wifi.preferStrongestKnown = doc["wifi"]["prefer_strongest_known"] | sConfig->wifi.preferStrongestKnown;
      sConfig->wifi.periodicScanEnabled = doc["wifi"]["periodic_scan_enabled"] | sConfig->wifi.periodicScanEnabled;
      sConfig->wifi.periodicScanIntervalSeconds = doc["wifi"]["periodic_scan_interval_seconds"] | sConfig->wifi.periodicScanIntervalSeconds;
    }

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
    sConfig->notifications.type = doc["notifications"]["type"] | sConfig->notifications.type;
    sConfig->notifications.webhookUrl = doc["notifications"]["webhook_url"] | sConfig->notifications.webhookUrl;
    sConfig->notifications.webhookMethod = doc["notifications"]["webhook_method"] | sConfig->notifications.webhookMethod;
    sConfig->notifications.webhookAuthToken = doc["notifications"]["webhook_auth_token"] | sConfig->notifications.webhookAuthToken;
    sConfig->notifications.sendOnTrigger = doc["notifications"]["send_on_trigger"] | sConfig->notifications.sendOnTrigger;
    sConfig->notifications.sendOnRecovery = doc["notifications"]["send_on_recovery"] | sConfig->notifications.sendOnRecovery;
    sConfig->notifications.sendOnMaxCyclesReached = doc["notifications"]["send_on_max_cycles_reached"] | sConfig->notifications.sendOnMaxCyclesReached;
    sConfig->notifications.sendTestNotificationEnabled = doc["notifications"]["send_test_notification_enabled"] | sConfig->notifications.sendTestNotificationEnabled;

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
    sConfig->central.deviceId = doc["central"]["device_id"] | sConfig->central.deviceId;
    sConfig->central.deviceToken = doc["central"]["device_token"] | sConfig->central.deviceToken;
    sConfig->central.pollIntervalSeconds = doc["central"]["poll_interval_seconds"] | sConfig->central.pollIntervalSeconds;
    sConfig->central.heartbeatIntervalSeconds = doc["central"]["heartbeat_interval_seconds"] | sConfig->central.heartbeatIntervalSeconds;
    sConfig->power.enabled = doc["power"]["enabled"] | sConfig->power.enabled;
    sConfig->power.sampleRateHz = doc["power"]["sample_rate_hz"] | sConfig->power.sampleRateHz;
    sConfig->power.batchSeconds = doc["power"]["batch_seconds"] | sConfig->power.batchSeconds;
    sConfig->power.includeWifiStats = doc["power"]["include_wifi_stats"] | sConfig->power.includeWifiStats;
    // include_frequency intentionally not accepted: no real frequency value is ever produced.

    sConfig->discovery.mdnsEnabled = doc["discovery"]["mdns_enabled"] | sConfig->discovery.mdnsEnabled;
    sConfig->discovery.udpAnnounceEnabled = doc["discovery"]["udp_announce_enabled"] | sConfig->discovery.udpAnnounceEnabled;
    sConfig->discovery.udpPort = doc["discovery"]["udp_port"] | sConfig->discovery.udpPort;

    if (!sConfig->central.enrollmentToken.isEmpty() && sConfig->central.enrollmentToken != previousEnrollmentToken) {
      sConfig->central.deviceId = "";
      sConfig->central.deviceToken = "";
    }

    sCfgMgr->save(*sConfig);
    sMonitor->resetIncident();
    sEventLog->add("config", "Configuration saved by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });


  server.on("/api/wifi/scan", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!sWifi) {
      server.send(503, "application/json", "{\"error\":\"wifi service unavailable\"}");
      return;
    }
    // Scanning briefly disrupts the link; it is gated behind an explicit
    // protected action and runs synchronously, freeing the result immediately.
    String networks = sWifi->scanNetworksJson();
    String out = "{\"networks\":";
    out += networks;
    out += "}";
    server.send(200, "application/json", out);
  });
  server.on("/api/wifi/scan", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  server.on("/api/system/reboot", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Reboot requested by API");
    sEventLog->flush();
    sCfgMgr->prepareForPlannedRestart("api_reboot");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(100);
    ESP.restart();
  });

  server.on("/api/system/recovery-boot", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Recovery boot requested by API");
    sEventLog->flush();
    sCfgMgr->prepareForPlannedRestart("api_recovery_boot");
    sCfgMgr->requestRecoveryBoot();
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true,\"recovery_boot\":true}");
    delay(100);
    ESP.restart();
  });

  server.on("/api/system/factory-reset", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Factory reset requested by API");
    sEventLog->flush();
    if (sWifi) {
      sWifi->clearProvisionedCredentials();
    }
    CrashRecorder::clearStoredCrashes();
    sCfgMgr->reset();
    sCfgMgr->prepareForPlannedRestart("api_factory_reset");
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
    sCfgMgr->prepareForPlannedRestart("api_ota_finalize");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(250);
    ESP.restart();
  }, []() {
    if (sAuth && !sAuth->isAuthorized(server)) return;
    HTTPUpload& upload = server.upload();
    sOta->handleUpload(upload);
  });

  server.on("/api/relay/on", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/relay/off", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/relay/toggle", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/config/save", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/reboot", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/recovery-boot", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/factory-reset", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/ota", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204, "text/plain", "");
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
