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
      // Only send a password when the user typed one; omitting it keeps the
      // stored password for a known SSID (per the config/save contract).
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
  if (state.status.last_crash_present) {
    $('crash-summary').textContent = `A crash was recorded: ${state.status.last_crash_reason || 'unknown'}. Use Refresh Crash Dumps for details.`;
  } else {
    $('crash-summary').textContent = 'No crash recorded.';
  }
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

async function refreshCrashDumps() {
  try {
    const dumps = await fetchJson('/api/system/crash');
    if (Array.isArray(dumps) && dumps.length) {
      $('crash-view').textContent = JSON.stringify(dumps, null, 2);
      logMessage(`Loaded ${dumps.length} stored crash dump(s).`);
    } else {
      $('crash-view').textContent = 'No crash dumps stored on the device.';
      logMessage('No crash dumps stored.');
    }
  } catch (error) {
    $('crash-view').textContent = `Failed to load crash dumps: ${error.message}`;
    logMessage(`Crash dump load failed: ${error.message}`);
  }
}

async function handleCrashClear() {
  try {
    await postJson('/api/system/crash/clear', {});
    $('crash-view').textContent = 'Crash dumps cleared.';
    logMessage('Stored crash dumps cleared.');
    await refreshStatus();
  } catch (error) {
    logMessage(`Crash clear failed: ${error.message}`);
  }
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

  // Only enabled + base_urls are user-editable here. Protected identity fields
  // (enrollment_token, device_id, device_token, site_id) are intentionally
  // omitted so the device keeps its stored values.
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
  $('crash-refresh').addEventListener('click', refreshCrashDumps);
  $('crash-clear').addEventListener('click', handleCrashClear);
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
