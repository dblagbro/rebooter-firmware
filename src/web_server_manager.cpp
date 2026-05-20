#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "web_server_manager.h"
#include "relay_controller.h"
#include "config_manager.h"
#include "event_log.h"
#include "monitor_engine.h"
#include "ota_manager.h"
#include "auth_manager.h"
#include "firmware_version.h"
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266httpUpdate.h>

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
          <div><dt>Central</dt><dd id="central-state">-</dd></div>
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
        <h2>Settings</h2>
        <form id="config-form" class="stack">
          <label><span>Device Name <a class="tt" title="Operator-friendly name shown in hub UI and notifications. 1-32 characters.">?</a></span><input id="cfg-device-name" maxlength="32"></label>
          <label>
            <span>Mode <a class="tt" title="Smart Plug = manual relay control only. Internet Watchdog = auto-restart when internet targets fail. Device Watchdog = auto-restart when a single LAN device is unreachable.">?</a></span>
            <select id="cfg-mode">
              <option value="smart_plug">Smart Plug</option>
              <option value="internet_watchdog">Internet Watchdog</option>
              <option value="device_watchdog">Device Watchdog</option>
            </select>
          </label>
          <label>
            <span>Relay Restore <a class="tt" title="What the relay does on power-on. Restore Previous remembers last state across boots. Default: Restore Previous.">?</a></span>
            <select id="cfg-restore">
              <option value="restore_previous">Restore Previous</option>
              <option value="always_on">Always On</option>
              <option value="always_off">Always Off</option>
            </select>
          </label>
          <label><span>Monitor Interval (sec) <a class="tt" title="How often the device probes its monitor target(s). Lower = faster detection, more traffic. Range 2-3600, default 5. Not used in Smart Plug mode.">?</a></span><input id="cfg-monitor-interval" type="number" min="2" max="3600" placeholder="default 5"></label>
          <label><span>Boot Warm-up (sec) <a class="tt" title="Grace period after relay-on before counting monitor failures. Lets the downstream device boot. Range 0-600, default 30.">?</a></span><input id="cfg-boot-warmup" type="number" min="0" max="600" placeholder="default 30"></label>
          <label class="checkbox-row"><input id="cfg-manual-button" type="checkbox"><span>Enable button relay toggle <a class="tt" title="Whether short-press on the physical button toggles the relay. Smart Plug mode only. Long-press behaviors always work.">?</a></span></label>
          <label><span>Admin Username</span><input id="cfg-admin-username" maxlength="32" placeholder="admin"></label>
          <label><span>Admin Password <a class="tt" title="Credentials for local web UI authenticated endpoints. Password must be at least 8 characters. Leave blank to keep current.">?</a></span><input id="cfg-admin-password" type="password" minlength="8" maxlength="64" placeholder="Leave blank to keep current"></label>
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
        <p class="hint">Local OTA endpoint. No serial cable needed after bootstrap.</p>
      </article>
    </section>

    <section class="band" id="iw-panel" style="display:none">
      <article class="panel">
        <h2>Internet Watchdog Settings</h2>
        <form id="iw-form" class="stack">
          <label><span>Targets <a class="tt" title="1-10 IPs or hostnames to ping. All must fail before triggering a reboot. Pick 2-3 across different providers.">?</a></span>
            <div id="iw-targets-list"></div>
            <div class="actions"><button type="button" id="iw-add-target" class="secondary">+ Add Target</button></div>
          </label>
          <label><span>Failure Threshold (sec) <a class="tt" title="How many seconds ALL targets must be failing before power-cycling. Range 30-3600, default 180 (3 min).">?</a></span><input id="iw-failure-threshold" type="number" min="30" max="3600" placeholder="default 180"></label>
          <label><span>Power Off Duration (sec) <a class="tt" title="How long the relay stays OFF during a cycle. Most modems need 3-10 seconds. Range 1-60, default 5.">?</a></span><input id="iw-power-off" type="number" min="1" max="60" placeholder="default 5"></label>
          <label><span>Post-Reboot Holdoff (sec) <a class="tt" title="Grace window after cycle ends — failures ignored while device reboots. Range 30-1800, default 180.">?</a></span><input id="iw-holdoff" type="number" min="30" max="1800" placeholder="default 180"></label>
          <label><span>Max Cycles per Incident <a class="tt" title="Maximum reboots per outage before giving up. Prevents endless cycling. Range 1-10, default 3.">?</a></span><input id="iw-max-incident" type="number" min="1" max="10" placeholder="default 3"></label>
          <label><span>Max Cycles per Hour <a class="tt" title="Hard cap on reboots per hour. Prevents pathological reboot-loops. Range 1-20, default 6.">?</a></span><input id="iw-max-hour" type="number" min="1" max="20" placeholder="default 6"></label>
          <label><span>Cooldown (sec) <a class="tt" title="Lockout period after giving up on an incident. Manual override via Smart Plug toggle. Range 60-86400, default 3600 (1h).">?</a></span><input id="iw-cooldown" type="number" min="60" max="86400" placeholder="default 3600"></label>
          <label><span>DNS Refresh (sec) <a class="tt" title="How often hostname targets are re-resolved. Catches target migration. Range 60-86400, default 300.">?</a></span><input id="iw-dns-refresh" type="number" min="60" max="86400" placeholder="default 300"></label>
          <label><span>Recovery Stability (sec) <a class="tt" title="How long the network must stay healthy before declaring the incident resolved. Range 5-300, default 15.">?</a></span><input id="iw-recovery" type="number" min="5" max="300" placeholder="default 15"></label>
          <div class="actions"><button type="submit">Save Watchdog Settings</button></div>
        </form>
      </article>
    </section>

    <section class="band" id="dw-panel" style="display:none">
      <article class="panel">
        <h2>Device Watchdog Settings</h2>
        <form id="dw-form" class="stack">
          <label><span>Target <a class="tt" title="IP or hostname of the single device to monitor. Examples: NAS, IP camera, smart-home hub.">?</a></span><input id="dw-target" placeholder="e.g. 192.168.1.50"></label>
          <label><span>Failure Threshold (sec) <a class="tt" title="Seconds the target must be unreachable before power-cycling. Range 30-3600, default 60.">?</a></span><input id="dw-failure-threshold" type="number" min="30" max="3600" placeholder="default 60"></label>
          <label><span>Power Off Duration (sec) <a class="tt" title="How long the relay stays OFF during a cycle. Range 1-60, default 5.">?</a></span><input id="dw-power-off" type="number" min="1" max="60" placeholder="default 5"></label>
          <label><span>Post-Reboot Holdoff (sec) <a class="tt" title="Grace window after cycle. Single appliances often take longer to boot. Range 30-1800, default 300.">?</a></span><input id="dw-holdoff" type="number" min="30" max="1800" placeholder="default 300"></label>
          <label><span>Max Cycles per Incident <a class="tt" title="Maximum reboots per outage before giving up. Range 1-10, default 3.">?</a></span><input id="dw-max-incident" type="number" min="1" max="10" placeholder="default 3"></label>
          <label><span>Max Cycles per Hour <a class="tt" title="Hard cap on reboots per hour. Range 1-20, default 6.">?</a></span><input id="dw-max-hour" type="number" min="1" max="20" placeholder="default 6"></label>
          <label><span>Cooldown (sec) <a class="tt" title="Lockout after giving up on an incident. Range 60-86400, default 3600 (1h).">?</a></span><input id="dw-cooldown" type="number" min="60" max="86400" placeholder="default 3600"></label>
          <label><span>Recovery Stability (sec) <a class="tt" title="How long the target must stay reachable before declaring resolved. Range 5-300, default 30.">?</a></span><input id="dw-recovery" type="number" min="5" max="300" placeholder="default 30"></label>
          <div class="actions"><button type="submit">Save Watchdog Settings</button></div>
        </form>
      </article>
    </section>

    <section class="band" id="notif-panel">
      <article class="panel">
        <h2>Notifications <a class="tt" title="Optional webhook or Pushover alerts for reboot events, recovery, and cycle-limit triggers.">?</a></h2>
        <form id="notif-form" class="stack">
          <label class="checkbox-row"><input id="notif-enabled" type="checkbox"><span>Enable Notifications</span></label>
          <label>
            <span>Type</span>
            <select id="notif-type">
              <option value="webhook">Webhook</option>
              <option value="pushover">Pushover</option>
            </select>
          </label>
          <div id="notif-webhook-fields">
            <label><span>Webhook URL <a class="tt" title="JSON payload is POSTed here on every trigger event.">?</a></span><input id="notif-webhook-url" type="url" placeholder="https://..."></label>
            <label>
              <span>Method</span>
              <select id="notif-webhook-method">
                <option value="POST">POST</option>
                <option value="PUT">PUT</option>
              </select>
            </label>
            <label><span>Auth Token <a class="tt" title="Sent as Authorization: Bearer header. Leave blank if not needed.">?</a></span><input id="notif-webhook-token" placeholder="Optional"></label>
          </div>
          <label class="checkbox-row"><input id="notif-on-trigger" type="checkbox" checked><span>Notify on trigger <a class="tt" title="Send notification when a reboot cycle is triggered.">?</a></span></label>
          <label class="checkbox-row"><input id="notif-on-recovery" type="checkbox" checked><span>Notify on recovery <a class="tt" title="Send notification when the monitored target recovers.">?</a></span></label>
          <label class="checkbox-row"><input id="notif-on-max-cycles" type="checkbox" checked><span>Notify on max cycles reached <a class="tt" title="Send notification when the device gives up after hitting the cycle limit.">?</a></span></label>
          <div class="actions">
            <button type="submit">Save Notifications</button>
            <button type="button" id="notif-test" class="secondary">Send Test</button>
          </div>
        </form>
      </article>
    </section>

    <section class="band grid-two">
      <article class="panel">
        <h2>Events</h2>
        <pre id="events-view">Loading events...</pre>
      </article>
      <article class="panel">
        <h2>System</h2>
        <div class="actions stack">
          <button id="btn-reboot" class="secondary">Reboot Device</button>
          <button id="btn-factory-reset" class="danger">Factory Reset</button>
        </div>
        <p class="hint" style="margin-top:14px">Message Log</p>
        <pre id="message-log">Ready.</pre>
      </article>
    </section>
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
button.danger { background: #7b2319; border-color: #7b2319; color: #fff; }
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
pre { margin: 0; padding: 14px; background: #f6f9f7; border: 1px solid #dee7e2; border-radius: 8px; white-space: pre-wrap; overflow-wrap: anywhere; font-size: 13px; max-height: 300px; overflow-y: auto; }
.tt { cursor: help; display: inline-block; width: 18px; height: 18px; line-height: 18px; text-align: center; border-radius: 50%; background: #d8e0dc; color: #124d39; font-size: 12px; font-weight: 700; text-decoration: none; vertical-align: middle; margin-left: 4px; }
.tt:hover { background: #124d39; color: #fff; }
.target-row { display: flex; gap: 8px; margin-bottom: 6px; }
.target-row input { flex: 1; }
.target-row button { padding: 8px 10px; font-size: 13px; }
#notif-webhook-fields { display: grid; gap: 12px; }
@media (max-width: 800px) { .grid-two { grid-template-columns: 1fr; } .topbar { flex-direction: column; } .topbar h1 { font-size: 28px; } }
)CSS";

static const char FALLBACK_APP_JS[] PROGMEM = R"JS(
const state={status:null,config:null,dirty:false};
const $=id=>document.getElementById(id);
function logMessage(m){const v=$('message-log');v.textContent=`[${new Date().toLocaleTimeString()}] ${m}\n`+v.textContent}
function fmtUp(s){s=Number(s||0);return `${Math.floor(s/86400)}d ${Math.floor((s%86400)/3600)}h ${Math.floor((s%3600)/60)}m ${s%60}s`}
function setHealthPill(h){const p=$('health-pill');p.textContent=h||'unknown';p.className=`status-pill ${String(h||'unknown').replace(/_/g,'-')}`}

function renderStatus(){
  if(!state.status)return;
  const s=state.status;
  $('device-name').textContent=s.device_name||'-';
  $('device-mode').textContent=(s.mode||'-').replace(/_/g,' ');
  $('relay-state').textContent=s.relay_on?'ON':'OFF';
  $('wifi-state').textContent=s.wifi_connected?'Connected':'Disconnected';
  $('device-ip').textContent=window.location.host||'-';
  $('uptime').textContent=fmtUp(s.uptime_seconds);
  $('central-state').textContent=s.central_enabled?(s.central_registered?'Registered ('+s.central_state+')':'Enabled, not registered'):'Disabled';
  $('connection-note').textContent=s.wifi_connected?'Device is online.':'Wi-Fi disconnected.';
  setHealthPill(s.health_state);
}

function showModePanels(mode){
  $('iw-panel').style.display=mode==='internet_watchdog'?'':'none';
  $('dw-panel').style.display=mode==='device_watchdog'?'':'none';
}

function addTargetRow(val){
  const list=$('iw-targets-list');
  const row=document.createElement('div');
  row.className='target-row';
  row.innerHTML=`<input type="text" value="${val||''}" placeholder="IP or hostname"><button type="button" class="secondary" onclick="this.parentElement.remove();state.dirty=true">X</button>`;
  list.appendChild(row);
}

function getTargets(){
  return Array.from(document.querySelectorAll('#iw-targets-list .target-row input')).map(i=>i.value.trim()).filter(Boolean);
}

function renderConfig(){
  if(!state.config)return;
  const c=state.config;
  $('cfg-device-name').value=c.device_name||'';
  $('cfg-mode').value=c.current_mode||'smart_plug';
  $('cfg-restore').value=c.relay_restore_behavior||'restore_previous';
  $('cfg-monitor-interval').value=c.monitor_interval_seconds??5;
  $('cfg-boot-warmup').value=c.boot_warmup_seconds??30;
  $('cfg-manual-button').checked=!!c.manual_button_enabled;
  $('cfg-admin-username').value=c.admin_username||'admin';
  showModePanels(c.current_mode);

  // Internet Watchdog
  const iw=c.internet||{};
  $('iw-targets-list').innerHTML='';
  (iw.targets||['1.1.1.1','8.8.8.8','time.nist.gov']).forEach(t=>addTargetRow(t));
  $('iw-failure-threshold').value=iw.failure_threshold_seconds??180;
  $('iw-power-off').value=iw.power_off_seconds??5;
  $('iw-holdoff').value=iw.post_reboot_holdoff_seconds??180;
  $('iw-max-incident').value=iw.max_cycles_per_incident??3;
  $('iw-max-hour').value=iw.max_cycles_per_hour??6;
  $('iw-cooldown').value=iw.cooldown_seconds??3600;
  $('iw-dns-refresh').value=iw.dns_refresh_seconds??300;
  $('iw-recovery').value=iw.recovery_stability_seconds??15;

  // Device Watchdog
  const dw=c.device||{};
  $('dw-target').value=dw.target||'';
  $('dw-failure-threshold').value=dw.failure_threshold_seconds??60;
  $('dw-power-off').value=dw.power_off_seconds??5;
  $('dw-holdoff').value=dw.post_reboot_holdoff_seconds??300;
  $('dw-max-incident').value=dw.max_cycles_per_incident??3;
  $('dw-max-hour').value=dw.max_cycles_per_hour??6;
  $('dw-cooldown').value=dw.cooldown_seconds??3600;
  $('dw-recovery').value=dw.recovery_stability_seconds??30;

  // Notifications
  const n=c.notifications||{};
  $('notif-enabled').checked=!!n.enabled;
  $('notif-type').value=n.type||'webhook';
  $('notif-webhook-url').value=n.webhook_url||'';
  $('notif-webhook-method').value=n.webhook_method||'POST';
  $('notif-webhook-token').value=n.webhook_auth_token||'';
  $('notif-on-trigger').checked=n.send_on_trigger!==false;
  $('notif-on-recovery').checked=n.send_on_recovery!==false;
  $('notif-on-max-cycles').checked=n.send_on_max_cycles_reached!==false;
  state.dirty=false;
}

async function fetchJson(path,opts){
  const r=await fetch(path,opts);const t=await r.text();
  let j={};if(t){try{j=JSON.parse(t)}catch(_){j={raw:t}}}
  if(!r.ok)throw new Error(j.error||`${r.status} ${r.statusText}`);return j;
}
async function postJson(p,d){return fetchJson(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})}
async function refreshStatus(){state.status=await fetchJson('/api/status');renderStatus()}
async function refreshConfig(){state.config=await fetchJson('/api/config');renderConfig()}
async function refreshEvents(){const e=await fetchJson('/api/events');$('events-view').textContent=JSON.stringify(e,null,2)}
async function refreshAll(){try{await Promise.all([refreshStatus(),refreshConfig(),refreshEvents()])}catch(e){logMessage('Refresh failed: '+e.message)}}

async function handleRelay(path,label){
  try{await fetchJson(path,{method:'POST'});logMessage(label+' sent.');await refreshStatus()}
  catch(e){logMessage(label+' failed: '+e.message)}
}

async function handleConfigSave(ev){
  ev.preventDefault();
  const p={device_name:$('cfg-device-name').value.trim(),current_mode:$('cfg-mode').value,
    relay_restore_behavior:$('cfg-restore').value,
    monitor_interval_seconds:Number($('cfg-monitor-interval').value||5),
    boot_warmup_seconds:Number($('cfg-boot-warmup').value||30),
    manual_button_enabled:$('cfg-manual-button').checked,
    admin_username:$('cfg-admin-username').value.trim()};
  const pw=$('cfg-admin-password').value;if(pw)p.admin_password=pw;
  if(state.config?.internet)p.internet=state.config.internet;
  if(state.config?.device)p.device=state.config.device;
  if(state.config?.notifications)p.notifications=state.config.notifications;
  try{await postJson('/api/config/save',p);$('cfg-admin-password').value='';state.dirty=false;logMessage('Settings saved.');await refreshAll()}
  catch(e){logMessage('Save failed: '+e.message)}
}

async function handleIwSave(ev){
  ev.preventDefault();
  const targets=getTargets();
  if(targets.length<1||targets.length>10){logMessage('Targets: 1-10 required.');return}
  const p={internet:{targets:targets,
    failure_threshold_seconds:Number($('iw-failure-threshold').value),
    power_off_seconds:Number($('iw-power-off').value),
    post_reboot_holdoff_seconds:Number($('iw-holdoff').value),
    max_cycles_per_incident:Number($('iw-max-incident').value),
    max_cycles_per_hour:Number($('iw-max-hour').value),
    cooldown_seconds:Number($('iw-cooldown').value),
    dns_refresh_seconds:Number($('iw-dns-refresh').value),
    recovery_stability_seconds:Number($('iw-recovery').value)}};
  try{await postJson('/api/config/save',p);state.dirty=false;logMessage('Internet Watchdog settings saved.');await refreshAll()}
  catch(e){logMessage('IW save failed: '+e.message)}
}

async function handleDwSave(ev){
  ev.preventDefault();
  const target=$('dw-target').value.trim();
  if(!target){logMessage('Target is required.');return}
  const p={device:{target:target,
    failure_threshold_seconds:Number($('dw-failure-threshold').value),
    power_off_seconds:Number($('dw-power-off').value),
    post_reboot_holdoff_seconds:Number($('dw-holdoff').value),
    max_cycles_per_incident:Number($('dw-max-incident').value),
    max_cycles_per_hour:Number($('dw-max-hour').value),
    cooldown_seconds:Number($('dw-cooldown').value),
    recovery_stability_seconds:Number($('dw-recovery').value)}};
  try{await postJson('/api/config/save',p);state.dirty=false;logMessage('Device Watchdog settings saved.');await refreshAll()}
  catch(e){logMessage('DW save failed: '+e.message)}
}

async function handleNotifSave(ev){
  ev.preventDefault();
  const p={notifications:{enabled:$('notif-enabled').checked,
    type:$('notif-type').value,
    webhook_url:$('notif-webhook-url').value.trim(),
    webhook_method:$('notif-webhook-method').value,
    webhook_auth_token:$('notif-webhook-token').value.trim(),
    send_on_trigger:$('notif-on-trigger').checked,
    send_on_recovery:$('notif-on-recovery').checked,
    send_on_max_cycles_reached:$('notif-on-max-cycles').checked}};
  try{await postJson('/api/config/save',p);state.dirty=false;logMessage('Notification settings saved.');await refreshAll()}
  catch(e){logMessage('Notification save failed: '+e.message)}
}

async function handleOtaSubmit(ev){
  ev.preventDefault();
  const f=$('ota-file').files&&$('ota-file').files[0];
  if(!f){logMessage('Choose a firmware .bin first.');return}
  const fd=new FormData();fd.append('update',f);
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/api/system/ota',true);
  xhr.upload.onprogress=e=>{if(!e.lengthComputable)return;const pct=Math.round((e.loaded/e.total)*100);$('ota-progress').value=pct;$('ota-progress-label').textContent=pct+'%'};
  xhr.onload=()=>{if(xhr.status<300){$('ota-progress').value=100;$('ota-progress-label').textContent='Done, rebooting';logMessage('OTA accepted. Rebooting.')}else{$('ota-progress-label').textContent='Failed ('+xhr.status+')';logMessage('OTA failed: '+(xhr.responseText||xhr.status))}};
  xhr.onerror=()=>{$('ota-progress-label').textContent='Network error';logMessage('OTA network error.')};
  $('ota-progress').value=0;$('ota-progress-label').textContent='Uploading...';
  logMessage('Uploading: '+f.name);xhr.send(fd);
}

function wireUi(){
  $('relay-on').addEventListener('click',()=>handleRelay('/api/relay/on','Relay on'));
  $('relay-off').addEventListener('click',()=>handleRelay('/api/relay/off','Relay off'));
  $('relay-toggle').addEventListener('click',()=>handleRelay('/api/relay/toggle','Relay toggle'));
  $('refresh-status').addEventListener('click',refreshAll);
  $('config-form').addEventListener('submit',handleConfigSave);
  $('iw-form').addEventListener('submit',handleIwSave);
  $('dw-form').addEventListener('submit',handleDwSave);
  $('notif-form').addEventListener('submit',handleNotifSave);
  $('ota-form').addEventListener('submit',handleOtaSubmit);
  $('iw-add-target').addEventListener('click',()=>{if(getTargets().length>=10){logMessage('Max 10 targets.');return}addTargetRow('');state.dirty=true});
  $('cfg-mode').addEventListener('change',function(){showModePanels(this.value);state.dirty=true});
  $('btn-reboot').addEventListener('click',async()=>{if(!confirm('Reboot the device?'))return;try{await postJson('/api/system/reboot',{});logMessage('Rebooting...')}catch(e){logMessage('Reboot failed: '+e.message)}});
  $('btn-factory-reset').addEventListener('click',async()=>{if(!confirm('FACTORY RESET? All settings will be erased.'))return;if(!confirm('Are you sure? This cannot be undone.'))return;try{await postJson('/api/system/factory-reset',{});logMessage('Factory reset initiated.')}catch(e){logMessage('Reset failed: '+e.message)}});
  $('notif-test').addEventListener('click',async()=>{logMessage('Test notification not yet implemented on device.');});
  document.querySelectorAll('input,select').forEach(el=>el.addEventListener('change',()=>{state.dirty=true}));
  window.addEventListener('beforeunload',e=>{if(state.dirty){e.preventDefault();e.returnValue=''}});
}
wireUi();refreshAll();setInterval(refreshStatus,5000);
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
  doc["notifications"]["webhook_auth_token"] = sConfig->notifications.webhookAuthToken;
  doc["notifications"]["send_on_trigger"] = sConfig->notifications.sendOnTrigger;
  doc["notifications"]["send_on_recovery"] = sConfig->notifications.sendOnRecovery;
  doc["notifications"]["send_on_max_cycles_reached"] = sConfig->notifications.sendOnMaxCyclesReached;
  doc["notifications"]["send_test_notification_enabled"] = sConfig->notifications.sendTestNotificationEnabled;

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
    doc["free_heap"] = ESP.getFreeHeap();
    doc["firmware_version"] = FIRMWARE_VERSION;
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

  // --- LAN Management Endpoints ---

  // Scan local subnet for other rebooter devices
  // Accepts optional ?start=X&end=Y query params to limit range (default 1-254)
  server.on("/api/lan/scan", HTTP_GET, []() {
    if (!requireAuth()) return;
    IPAddress localIp = WiFi.localIP();

    int rangeStart = server.hasArg("start") ? server.arg("start").toInt() : 1;
    int rangeEnd = server.hasArg("end") ? server.arg("end").toInt() : 254;
    if (rangeStart < 1) rangeStart = 1;
    if (rangeEnd > 254) rangeEnd = 254;
    // Limit to 50 IPs per request to avoid WDT reset
    if (rangeEnd - rangeStart > 50) rangeEnd = rangeStart + 50;

    // Build response as a string to avoid large JsonDocument
    String out = "{\"scanner_ip\":\"" + localIp.toString() + "\",\"devices\":[";
    int found = 0;

    for (int i = rangeStart; i <= rangeEnd; i++) {
      IPAddress target(localIp[0], localIp[1], localIp[2], i);
      if (target == localIp) continue;

      // Feed the watchdog between probes
      ESP.wdtFeed();
      yield();

      // Quick TCP connect
      WiFiClient probe;
      probe.setTimeout(1000);
      if (probe.connect(target, 80)) {
        probe.setTimeout(2000);
        probe.print("GET /api/status HTTP/1.0\r\nHost: ");
        probe.print(target.toString());
        probe.print("\r\nConnection: close\r\n\r\n");

        String body = "";
        uint32_t start = millis();
        bool inBody = false;
        while (probe.connected() && millis() - start < 3000) {
          while (probe.available()) {
            char c = probe.read();
            if (inBody) body += c;
            // Simple header skip — detect blank line
            if (!inBody && body.length() < 4) body += c;
            if (!inBody && body.endsWith("\r\n\r\n")) { inBody = true; body = ""; }
          }
          if (body.length() > 600) break; // limit memory
          yield();
        }
        probe.stop();

        if (found > 0) out += ",";
        out += "{\"ip\":\"" + target.toString() + "\"";

        // Try to extract device_name from JSON
        JsonDocument peer;
        if (body.length() > 2 && deserializeJson(peer, body) == DeserializationError::Ok && peer.containsKey("device_name")) {
          out += ",\"is_rebooter\":true";
          out += ",\"device_name\":\"" + String(peer["device_name"] | "?") + "\"";
          out += ",\"firmware_version\":\"" + String(peer["firmware_version"] | "?") + "\"";
          out += ",\"mode\":\"" + String(peer["mode"] | "?") + "\"";
          out += ",\"relay_on\":" + String((bool)(peer["relay_on"] | false) ? "true" : "false");
          out += ",\"central_state\":\"" + String(peer["central_state"] | "?") + "\"";
        } else {
          out += ",\"is_rebooter\":false,\"http\":true";
        }
        out += "}";
        found++;
      }
      yield();
    }
    out += "],\"count\":" + String(found) + "}";
    server.send(200, "application/json", out);
    if (sEventLog) sEventLog->add("lan", "LAN scan found " + String(found) + " device(s)");
  });

  // Proxy a request to another LAN device
  server.on("/api/lan/proxy", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"missing body\"}");
      return;
    }

    JsonDocument req;
    if (deserializeJson(req, server.arg("plain")) != DeserializationError::Ok) {
      server.send(400, "application/json", "{\"error\":\"invalid json\"}");
      return;
    }

    const String targetIp = req["ip"] | "";
    const String path = req["path"] | "/api/status";
    const String method = req["method"] | "GET";
    const String body = req["body"] | "";

    if (targetIp.isEmpty()) {
      server.send(400, "application/json", "{\"error\":\"ip required\"}");
      return;
    }

    // Only allow LAN IPs (basic safety check)
    IPAddress target;
    if (!target.fromString(targetIp)) {
      server.send(400, "application/json", "{\"error\":\"invalid ip\"}");
      return;
    }

    WiFiClient client;
    HTTPClient http;
    String url = "http://" + targetIp + path;

    if (!http.begin(client, url)) {
      server.send(502, "application/json", "{\"error\":\"connection failed\"}");
      return;
    }

    http.setTimeout(10000);
    if (!body.isEmpty()) {
      http.addHeader("Content-Type", "application/json");
    }

    int httpCode;
    if (method == "POST") {
      httpCode = http.POST(body);
    } else {
      httpCode = http.GET();
    }

    String response = http.getString();
    http.end();

    JsonDocument doc;
    doc["target_ip"] = targetIp;
    doc["path"] = path;
    doc["http_code"] = httpCode;
    // Try to parse response as JSON, otherwise return as string
    JsonDocument respDoc;
    if (deserializeJson(respDoc, response) == DeserializationError::Ok) {
      doc["response"] = respDoc;
    } else {
      doc["response_text"] = response.substring(0, 512);
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
    if (sEventLog) sEventLog->add("lan", "Proxy " + method + " " + url + " -> " + String(httpCode));
  });

  // Tell this device to pull and flash firmware from a URL (HTTP only for LAN)
  server.on("/api/system/ota-pull", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"missing body\"}");
      return;
    }

    JsonDocument req;
    if (deserializeJson(req, server.arg("plain")) != DeserializationError::Ok) {
      server.send(400, "application/json", "{\"error\":\"invalid json\"}");
      return;
    }

    const String url = req["url"] | "";
    if (url.isEmpty()) {
      server.send(400, "application/json", "{\"error\":\"url required\"}");
      return;
    }

    server.send(200, "application/json", "{\"ok\":true,\"message\":\"OTA pull starting\"}");
    if (sEventLog) sEventLog->add("ota", "OTA pull from: " + url);

    delay(100);

    WiFiClient client;
    ESPhttpUpdate.rebootOnUpdate(true);
    ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    t_httpUpdate_return result = ESPhttpUpdate.update(client, url);

    // If we get here, the update failed (success would have rebooted)
    if (sEventLog) sEventLog->add("ota", "OTA pull failed: " + ESPhttpUpdate.getLastErrorString());
  });

  server.begin();
}

void WebServerManager::loop() {
  server.handleClient();
}
