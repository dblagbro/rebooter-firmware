param(
  [string]$DeviceBaseUrl = "http://192.168.1.48",
  [string]$OutFile = "",
  [int]$IntervalMs = 500,
  [int]$DurationSec = 120
)

if ([string]::IsNullOrWhiteSpace($OutFile)) {
  $stamp = Get-Date -Format "yyyy-MM-dd-HHmmss"
  $OutFile = "C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\button-test-status-$stamp.ndjson"
}

$deadline = (Get-Date).AddSeconds($DurationSec)
$uri = "$DeviceBaseUrl/api/status"

while ((Get-Date) -lt $deadline) {
  $capturedAt = Get-Date -Format o
  try {
    $raw = Invoke-WebRequest -UseBasicParsing -Uri $uri -TimeoutSec 5
    $status = $raw.Content | ConvertFrom-Json
    $row = [ordered]@{
      captured_at = $capturedAt
      ok = $true
      device_name = $status.device_name
      firmware_version = $status.firmware_version
      relay_on = $status.relay_on
      wifi_connected = $status.wifi_connected
      in_captive_portal = $status.in_captive_portal
      recovery_mode = $status.recovery_mode
      auto_recovery_triggered = $status.auto_recovery_triggered
      last_known_good_restored = $status.last_known_good_restored
      consecutive_unhealthy_boots = $status.consecutive_unhealthy_boots
      health_state = $status.health_state
      uptime_seconds = $status.uptime_seconds
      free_heap = $status.free_heap
      holdoff_remaining_seconds = $status.holdoff_remaining_seconds
      cooldown_remaining_seconds = $status.cooldown_remaining_seconds
      central_enabled = $status.central_enabled
      central_registered = $status.central_registered
      central_state = $status.central_state
      central_heartbeat_age_seconds = $status.central_heartbeat_age_seconds
    }
  } catch {
    $row = [ordered]@{
      captured_at = $capturedAt
      ok = $false
      error = $_.Exception.Message
    }
  }

  ($row | ConvertTo-Json -Compress) | Add-Content -Path $OutFile
  Start-Sleep -Milliseconds $IntervalMs
}

Write-Output $OutFile
