param(
  [string[]]$BaseUrls = @("http://192.168.1.69", "http://192.168.1.225"),
  [int]$DurationSeconds = 1800,
  [int]$IntervalMs = 1000,
  [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"

$expandedBaseUrls = New-Object System.Collections.Generic.List[string]
foreach ($entry in $BaseUrls) {
  foreach ($value in ($entry -split ',')) {
    $trimmed = $value.Trim()
    if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
      $expandedBaseUrls.Add($trimmed)
    }
  }
}
$BaseUrls = @($expandedBaseUrls)

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $OutputPath = "C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\g2-time-sync-$stamp.ndjson"
}

$outputDir = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDir)) {
  New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

function Get-UnixMs {
  return [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
}

function Invoke-StatusSample {
  param(
    [string]$BaseUrl
  )

  $beforeMs = Get-UnixMs
  try {
    $status = Invoke-RestMethod -Uri "$BaseUrl/api/status" -TimeoutSec 5
    $afterMs = Get-UnixMs
    $midpointMs = [int64](($beforeMs + $afterMs) / 2)
    $deviceWallClockMs = if ($status.time_synced -and $status.wall_clock_unix_ms) { [int64]$status.wall_clock_unix_ms } else { 0 }
    $offsetMs = if ($deviceWallClockMs -gt 0) { $deviceWallClockMs - $midpointMs } else { $null }
    $powerLastSampleUnixMs = 0
    if ($null -ne $status.power_last_sample_unix_ms) {
      $powerLastSampleUnixMs = [int64]$status.power_last_sample_unix_ms
    }
    return [pscustomobject]@{
      ok = $true
      device = $BaseUrl
      host_before_unix_ms = $beforeMs
      host_after_unix_ms = $afterMs
      host_midpoint_unix_ms = $midpointMs
      http_rtt_ms = ($afterMs - $beforeMs)
      time_synced = [bool]$status.time_synced
      device_wall_clock_unix_ms = $deviceWallClockMs
      offset_ms = $offsetMs
      uptime_seconds = [uint32]$status.uptime_seconds
      firmware_version = [string]$status.firmware_version
      recovery_mode = [bool]$status.recovery_mode
      health_state = [string]$status.health_state
      central_state = [string]$status.central_state
      central_last_heartbeat_uptime_seconds = [uint32]$status.central_last_heartbeat_uptime_seconds
      power_last_sample_unix_ms = $powerLastSampleUnixMs
    }
  } catch {
    $afterMs = Get-UnixMs
    return [pscustomobject]@{
      ok = $false
      device = $BaseUrl
      host_before_unix_ms = $beforeMs
      host_after_unix_ms = $afterMs
      host_midpoint_unix_ms = [int64](($beforeMs + $afterMs) / 2)
      http_rtt_ms = ($afterMs - $beforeMs)
      error = $_.Exception.Message
    }
  }
}

$startedAt = Get-Date
$deadline = $startedAt.AddSeconds($DurationSeconds)
$sampleNo = 0

while ((Get-Date) -lt $deadline) {
  $sampleNo++
  $batchAt = (Get-Date).ToString("o")
  $rows = foreach ($baseUrl in $BaseUrls) {
    $sample = Invoke-StatusSample -BaseUrl $baseUrl
    $sample | Add-Member -NotePropertyName sampled_at -NotePropertyValue $batchAt
    $sample | Add-Member -NotePropertyName sample_no -NotePropertyValue $sampleNo
    $sample
  }

  foreach ($row in $rows) {
    ($row | ConvertTo-Json -Compress) | Add-Content -Path $OutputPath
  }

  Start-Sleep -Milliseconds $IntervalMs
}

[pscustomobject]@{
  started_at = $startedAt.ToString("o")
  finished_at = (Get-Date).ToString("o")
  duration_seconds = $DurationSeconds
  interval_ms = $IntervalMs
  devices = $BaseUrls
  output_path = $OutputPath
  samples = $sampleNo
} | ConvertTo-Json -Compress
