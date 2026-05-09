const state = {
  status: null,
  config: null,
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

function renderStatus() {
  if (!state.status) return;
  $('device-name').textContent = state.status.device_name || '-';
  $('device-mode').textContent = state.status.mode || '-';
  $('relay-state').textContent = state.status.relay_on ? 'On' : 'Off';
  $('wifi-state').textContent = state.status.wifi_connected ? 'Connected' : 'Disconnected';
  $('device-ip').textContent = window.location.host || '-';
  $('uptime').textContent = formatUptime(state.status.uptime_seconds);
  $('connection-note').textContent = state.status.wifi_connected
    ? 'Device is online and serving the local control plane.'
    : 'Device is up but Wi-Fi is not currently connected.';
  setHealthPill(state.status.health_state);
}

function renderConfig() {
  if (!state.config) return;
  $('cfg-device-name').value = state.config.device_name || '';
  $('cfg-mode').value = state.config.current_mode || 'smart_plug';
  $('cfg-restore').value = state.config.relay_restore_behavior || 'restore_previous';
  $('cfg-monitor-interval').value = state.config.monitor_interval_seconds ?? 5;
  $('cfg-boot-warmup').value = state.config.boot_warmup_seconds ?? 30;
  $('cfg-manual-button').checked = !!state.config.manual_button_enabled;
  $('cfg-admin-username').value = state.config.admin_username || 'admin';
}

async function fetchJson(path, options) {
  const response = await fetch(path, options);
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
  try {
    await Promise.all([refreshStatus(), refreshConfig(), refreshEvents()]);
  } catch (error) {
    logMessage(`Refresh failed: ${error.message}`);
  }
}

async function postJson(path, payload) {
  return fetchJson(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
}

async function handleRelay(path, label) {
  try {
    await fetchJson(path, { method: 'POST' });
    logMessage(`${label} command sent.`);
    await refreshStatus();
  } catch (error) {
    logMessage(`${label} failed: ${error.message}`);
  }
}

async function handleConfigSave(event) {
  event.preventDefault();
  const payload = {
    device_name: $('cfg-device-name').value.trim(),
    current_mode: $('cfg-mode').value,
    relay_restore_behavior: $('cfg-restore').value,
    monitor_interval_seconds: Number($('cfg-monitor-interval').value || 5),
    boot_warmup_seconds: Number($('cfg-boot-warmup').value || 30),
    manual_button_enabled: $('cfg-manual-button').checked,
    admin_username: $('cfg-admin-username').value.trim(),
  };

  const password = $('cfg-admin-password').value;
  if (password) payload.admin_password = password;

  if (state.config?.internet) payload.internet = state.config.internet;
  if (state.config?.device) payload.device = state.config.device;
  if (state.config?.notifications) payload.notifications = state.config.notifications;

  try {
    await postJson('/api/config/save', payload);
    $('cfg-admin-password').value = '';
    logMessage('Settings saved.');
    await refreshAll();
  } catch (error) {
    logMessage(`Save failed: ${error.message}`);
  }
}

async function handleOtaSubmit(event) {
  event.preventDefault();
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
  $('relay-on').addEventListener('click', () => handleRelay('/api/relay/on', 'Relay on'));
  $('relay-off').addEventListener('click', () => handleRelay('/api/relay/off', 'Relay off'));
  $('relay-toggle').addEventListener('click', () => handleRelay('/api/relay/toggle', 'Relay toggle'));
  $('refresh-status').addEventListener('click', refreshAll);
  $('config-form').addEventListener('submit', handleConfigSave);
  $('ota-form').addEventListener('submit', handleOtaSubmit);
}

wireUi();
refreshAll();
setInterval(refreshStatus, 5000);
