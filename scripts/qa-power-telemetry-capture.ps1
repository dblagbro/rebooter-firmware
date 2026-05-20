param(
  [string[]]$BaseUrls = @(
    "http://192.168.1.30",
    "http://192.168.1.67"
  ),
  [string]$AuthToken = "",
  [int]$DurationHours = 24,
  [int]$IntervalSeconds = 30,
  [int]$EventIntervalMinutes = 15,
  [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

if ($BaseUrls.Count -eq 0) {
  throw "At least one BaseUrl is required."
}

if ($DurationHours -le 0) {
  throw "DurationHours must be positive."
}

if ($IntervalSeconds -lt 5) {
  throw "IntervalSeconds must be at least 5."
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
  $timestamp = Get-Date -Format "yyyy-MM-dd-HHmmss"
  $OutputDir = Join-Path `
    "C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc" `
    "power-capture-$timestamp"
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$statusLogPath = Join-Path $OutputDir "status.ndjson"
$summaryPath = Join-Path $OutputDir "summary.json"
$metaPath = Join-Path $OutputDir "meta.json"
$errorLogPath = Join-Path $OutputDir "errors.ndjson"

$headers = @{}
if (-not [string]::IsNullOrWhiteSpace($AuthToken)) {
  $headers["X-Rebooter-Auth"] = $AuthToken
}

$meta = [ordered]@{
  started_at = (Get-Date).ToString("o")
  duration_hours = $DurationHours
  interval_seconds = $IntervalSeconds
  event_interval_minutes = $EventIntervalMinutes
  base_urls = $BaseUrls
}
$meta | ConvertTo-Json -Depth 6 | Set-Content -Path $metaPath

function Get-SafeHostTag {
  param([string]$BaseUrl)

  $uri = [System.Uri]$BaseUrl
  return ($uri.Host -replace '[^A-Za-z0-9\.-]', '_')
}

function Invoke-Json {
  param(
    [string]$Uri,
    [hashtable]$Headers = @{}
  )

  Invoke-RestMethod -Uri $Uri -Headers $Headers -TimeoutSec 10
}

function Write-NdjsonLine {
  param(
    [string]$Path,
    [object]$Value
  )

  ($Value | ConvertTo-Json -Depth 12 -Compress) | Add-Content -Path $Path
}

$deadline = (Get-Date).AddHours($DurationHours)
$nextEventCaptureAt = @{}
$latestSummary = [ordered]@{
  started_at = $meta.started_at
  output_dir = $OutputDir
  devices = @{}
}

foreach ($baseUrl in $BaseUrls) {
  $nextEventCaptureAt[$baseUrl] = Get-Date
}

while ((Get-Date) -lt $deadline) {
  $sampledAt = Get-Date

  foreach ($baseUrl in $BaseUrls) {
    $hostTag = Get-SafeHostTag -BaseUrl $baseUrl
    try {
      $status = Invoke-Json -Uri "$baseUrl/api/status"

      $sample = [ordered]@{
        sampled_at = $sampledAt.ToString("o")
        base_url = $baseUrl
        host_tag = $hostTag
        device_name = $status.device_name
        firmware_version = $status.firmware_version
        relay_on = $status.relay_on
        wifi_connected = $status.wifi_connected
        recovery_mode = $status.recovery_mode
        central_state = $status.central_state
        central_registered = $status.central_registered
        health_state = $status.health_state
        uptime_seconds = $status.uptime_seconds
        power_analytics_enabled = $status.power_analytics_enabled
        power_chip_type = $status.power_chip_type
        power_sample_rate_hz = $status.power_sample_rate_hz
        power_batch_seconds = $status.power_batch_seconds
        power_chip_seen = $status.power_chip_seen
        power_source = $status.power_source
        power_source_flags = $status.power_source_flags
        power_voltage_v = $status.power_voltage_v
        power_current_ma = $status.power_current_ma
        power_power_w = $status.power_power_w
        power_apparent_power_va = $status.power_apparent_power_va
        power_power_factor = $status.power_power_factor
        power_frequency_hz = $status.power_frequency_hz
        power_energy_wh = $status.power_energy_wh
        power_valid_frame_count = $status.power_valid_frame_count
        power_invalid_frame_count = $status.power_invalid_frame_count
        power_last_sample_age_seconds = $status.power_last_sample_age_seconds
      }

      Write-NdjsonLine -Path $statusLogPath -Value $sample

      $latestSummary.devices[$hostTag] = [ordered]@{
        base_url = $baseUrl
        sampled_at = $sample.sampled_at
        device_name = $sample.device_name
        firmware_version = $sample.firmware_version
        recovery_mode = $sample.recovery_mode
        central_state = $sample.central_state
        relay_on = $sample.relay_on
        power_source = $sample.power_source
        power_voltage_v = $sample.power_voltage_v
        power_current_ma = $sample.power_current_ma
        power_power_w = $sample.power_power_w
        power_valid_frame_count = $sample.power_valid_frame_count
        power_invalid_frame_count = $sample.power_invalid_frame_count
        power_last_sample_age_seconds = $sample.power_last_sample_age_seconds
      }

      if ($sampledAt -ge $nextEventCaptureAt[$baseUrl]) {
        try {
          $events = Invoke-Json -Uri "$baseUrl/api/events" -Headers $headers
          $eventsPath = Join-Path $OutputDir ("events-{0}-{1}.json" -f $hostTag, $sampledAt.ToString("yyyyMMdd-HHmmss"))
          $events | ConvertTo-Json -Depth 12 | Set-Content -Path $eventsPath
        } catch {
          $eventError = [ordered]@{
            sampled_at = $sampledAt.ToString("o")
            base_url = $baseUrl
            scope = "events"
            error = $_.Exception.Message
          }
          Write-NdjsonLine -Path $errorLogPath -Value $eventError
        }

        $nextEventCaptureAt[$baseUrl] = $sampledAt.AddMinutes($EventIntervalMinutes)
      }
    } catch {
      $errorEntry = [ordered]@{
        sampled_at = $sampledAt.ToString("o")
        base_url = $baseUrl
        scope = "status"
        error = $_.Exception.Message
      }
      Write-NdjsonLine -Path $errorLogPath -Value $errorEntry

      $latestSummary.devices[$hostTag] = [ordered]@{
        base_url = $baseUrl
        sampled_at = $sampledAt.ToString("o")
        error = $_.Exception.Message
      }
    }
  }

  $latestSummary.last_updated_at = (Get-Date).ToString("o")
  $latestSummary | ConvertTo-Json -Depth 10 | Set-Content -Path $summaryPath
  Start-Sleep -Seconds $IntervalSeconds
}

$latestSummary.completed_at = (Get-Date).ToString("o")
$latestSummary | ConvertTo-Json -Depth 10 | Set-Content -Path $summaryPath
