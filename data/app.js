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

function splitTargets(value) {
  return String(value || '')
    .split(/\r?\n/)
    .map((entry) => entry.trim())
    .filter(Boolean);
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
  const response = await fetch(path, {
    cache: 'no-store',
    ...options,
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

async function handleRelay(path, label) {
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
  const payload = {
    device_name: $('cfg-device-name').value.trim(),
    current_mode: $('cfg-mode').value,
    relay_restore_behavior: $('cfg-restore').value,
    monitor_interval_seconds: Number($('cfg-monitor-interval').value || 5),
    boot_warmup_seconds: Number($('cfg-boot-warmup').value || 30),
    manual_button_enabled: $('cfg-manual-button').checked,
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

  if (state.config?.notifications) payload.notifications = state.config.notifications;
  if (state.config?.central) payload.central = state.config.central;

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
  $('cfg-mode').addEventListener('change', renderModeSections);
  $('config-form').addEventListener('submit', handleConfigSave);
  $('ota-form').addEventListener('submit', handleOtaSubmit);
}

wireUi();
refreshAll();
setInterval(() => {
  refreshStatus().catch((error) => logMessage(`Background status refresh failed: ${error.message}`));
}, 5000);
