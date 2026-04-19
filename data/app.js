async function refreshStatus() {
  const res = await fetch('/api/status');
  const data = await res.json();
  document.getElementById('status').innerText = JSON.stringify(data, null, 2);
}
async function relayOn() { await fetch('/api/relay/on', { method: 'POST' }); refreshStatus(); }
async function relayOff() { await fetch('/api/relay/off', { method: 'POST' }); refreshStatus(); }
refreshStatus();
setInterval(refreshStatus, 5000);

