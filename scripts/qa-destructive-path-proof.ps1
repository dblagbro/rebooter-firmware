param(
  [string]$BaseUrl = "http://192.168.1.48",
  [string]$AuthToken = "",
  [string]$RestoreAdminPassword = "BenchPass123!",
  [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutDir)) {
  $stamp = Get-Date -Format "yyyy-MM-dd-HHmmss"
  $OutDir = "C:\Users\Administrator\Documents\Codex\2026-04-18-all-projets-on-this-windows-pc\destructive-proof-$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Invoke-Request {
  param(
    [string]$Method,
    [string]$Path,
    [hashtable]$Headers = @{},
    [string]$Body = $null,
    [string]$ContentType = "application/json",
    [int]$TimeoutSec = 15
  )

  try {
    $params = @{
      Uri = "$BaseUrl$Path"
      Method = $Method
      Headers = $Headers
      UseBasicParsing = $true
      TimeoutSec = $TimeoutSec
    }
    if (($Method -ne "GET") -and ($null -ne $Body)) {
      $params.Body = $Body
      $params.ContentType = $ContentType
    }
    $resp = Invoke-WebRequest @params
    return [pscustomobject]@{
      ok = $true
      status = [int]$resp.StatusCode
      body = [string]$resp.Content
    }
  } catch {
    $resp = $_.Exception.Response
    if ($resp) {
      $reader = New-Object System.IO.StreamReader($resp.GetResponseStream())
      $content = $reader.ReadToEnd()
      $reader.Close()
      return [pscustomobject]@{
        ok = $false
        status = [int]$resp.StatusCode
        body = [string]$content
      }
    }
    return [pscustomobject]@{
      ok = $false
      status = -1
      body = [string]$_.Exception.Message
    }
  }
}

function Parse-JsonSafe {
  param([string]$Body)
  if ([string]::IsNullOrWhiteSpace($Body)) { return $null }
  try { return $Body | ConvertFrom-Json -Depth 20 } catch { return $null }
}

function Save-JsonFile {
  param([string]$Path, $Value)
  $json = $Value | ConvertTo-Json -Depth 20
  Set-Content -Path $Path -Value $json
}

function Get-Status {
  return Parse-JsonSafe((Invoke-Request -Method GET -Path "/api/status" -Headers @{} -Body $null -ContentType "application/json" -TimeoutSec 10).body)
}

function Get-Events {
  $parsed = Parse-JsonSafe((Invoke-Request -Method GET -Path "/api/events" -Headers @{} -Body $null -ContentType "application/json" -TimeoutSec 10).body)
  if ($parsed -is [System.Array]) { return @($parsed) }
  if ($null -eq $parsed) { return @() }
  return @($parsed)
}

function Any-ReachableSample {
  param($Samples)
  return (@($Samples | Where-Object { $_.ok }).Count -gt 0)
}

function Collect-StatusWindow {
  param(
    [string]$Name,
    [int]$DurationSec,
    [int]$IntervalMs = 500
  )

  $samples = New-Object System.Collections.Generic.List[object]
  $deadline = (Get-Date).AddSeconds($DurationSec)
  while ((Get-Date) -lt $deadline) {
    $capturedAt = Get-Date -Format o
    $resp = Invoke-Request GET "/api/status" @{} $null "application/json" 5
    if ($resp.ok -and $resp.status -eq 200) {
      $status = Parse-JsonSafe $resp.body
      $samples.Add([ordered]@{
        captured_at = $capturedAt
        ok = $true
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
        auth_required = $status.auth_required
        central_enabled = $status.central_enabled
        central_registered = $status.central_registered
        central_state = $status.central_state
        central_identity_present = $status.central_identity_present
      })
    } else {
      $samples.Add([ordered]@{
        captured_at = $capturedAt
        ok = $false
        status = $resp.status
        error = $resp.body
      })
    }
    Start-Sleep -Milliseconds $IntervalMs
  }

  $path = Join-Path $OutDir "$Name-status-window.json"
  Save-JsonFile -Path $path -Value $samples
  return $samples
}

function Summarize-Window {
  param($Samples)
  $reachable = @($Samples | Where-Object { $_.ok })
  $unreachableCount = @($Samples | Where-Object { -not $_.ok }).Count
  $firstReachable = if ($reachable.Count -gt 0) { $reachable[0] } else { $null }
  $lastReachable = if ($reachable.Count -gt 0) { $reachable[$reachable.Count - 1] } else { $null }
  return [ordered]@{
    samples = $Samples.Count
    unreachable_samples = $unreachableCount
    first_reachable = $firstReachable
    last_reachable = $lastReachable
    saw_recovery_mode = (@($reachable | Where-Object { $_.recovery_mode }).Count -gt 0)
    saw_last_known_good_restored = (@($reachable | Where-Object { $_.last_known_good_restored }).Count -gt 0)
    saw_healthy = (@($reachable | Where-Object { $_.health_state -eq "healthy" }).Count -gt 0)
  }
}

function Wait-ForHealthy {
  param(
    [int]$TimeoutSec = 180
  )
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    $status = Get-Status
    if ($null -ne $status -and $status.health_state -eq "healthy") {
      return $status
    }
    Start-Sleep -Seconds 2
  }
  return Get-Status
}

$goodHeaders = @{}
if (-not [string]::IsNullOrWhiteSpace($AuthToken)) {
  $goodHeaders["X-Rebooter-Auth"] = $AuthToken
}

$result = [ordered]@{
  date = (Get-Date).ToString("s")
  base_url = $BaseUrl
  out_dir = $OutDir
  restore_admin_password = "<redacted>"
}

$baselineStatus = Get-Status
if ($null -eq $baselineStatus) {
  throw "Baseline status could not be read from $BaseUrl"
}
if ($baselineStatus.auth_required -and [string]::IsNullOrWhiteSpace($AuthToken)) {
  throw "This device requires the local admin password. Re-run with -AuthToken."
}
$baselineEvents = Get-Events
$baselineBackupResp = Invoke-Request -Method GET -Path "/api/system/config-backup" -Headers $goodHeaders -Body $null -ContentType "application/json" -TimeoutSec 20
$baselineBackup = Parse-JsonSafe $baselineBackupResp.body
if (-not $baselineBackupResp.ok -or $null -eq $baselineBackup) {
  throw "Protected config backup could not be captured before destructive testing."
}

Save-JsonFile -Path (Join-Path $OutDir "baseline-status.json") -Value $baselineStatus
Save-JsonFile -Path (Join-Path $OutDir "baseline-events.json") -Value $baselineEvents
Save-JsonFile -Path (Join-Path $OutDir "baseline-config-backup.json") -Value $baselineBackup

$result["baseline"] = [ordered]@{
  status = $baselineStatus
  event_count = $baselineEvents.Count
}

# 1. Normal reboot proof
$rebootResp = Invoke-Request -Method POST -Path "/api/system/reboot" -Headers $goodHeaders
$rebootSamples = Collect-StatusWindow -Name "reboot" -DurationSec 120
$rebootFinal = Wait-ForHealthy -TimeoutSec 60
$result["reboot"] = [ordered]@{
  trigger = $rebootResp
  summary = Summarize-Window $rebootSamples
  final_status = $rebootFinal
}

# 2. Recovery-boot proof
$recoveryResp = Invoke-Request -Method POST -Path "/api/system/recovery-boot" -Headers $goodHeaders
$recoverySamples = Collect-StatusWindow -Name "recovery-boot" -DurationSec 90
$recoveryFinal = Get-Status
$result["recovery_boot"] = [ordered]@{
  trigger = $recoveryResp
  summary = Summarize-Window $recoverySamples
  final_status = $recoveryFinal
}

if (-not (Any-ReachableSample $recoverySamples) -or ($null -eq $recoveryFinal) -or $recoveryFinal.recovery_mode) {
  $result["manual_action_required"] = [ordered]@{
    phase = "recovery_boot"
    reason = "Device entered or remained in recovery mode and requires manual provisioning/reboot before further destructive steps."
  }
  Save-JsonFile -Path (Join-Path $OutDir "destructive-proof-summary.json") -Value $result
  $result | ConvertTo-Json -Depth 20
  return
}

# Leave recovery mode cleanly.
$exitRecoveryResp = Invoke-Request -Method POST -Path "/api/system/reboot" -Headers $goodHeaders
$exitRecoverySamples = Collect-StatusWindow -Name "recovery-exit-reboot" -DurationSec 120
$exitRecoveryFinal = Wait-ForHealthy -TimeoutSec 60
$result["recovery_exit"] = [ordered]@{
  trigger = $exitRecoveryResp
  summary = Summarize-Window $exitRecoverySamples
  final_status = $exitRecoveryFinal
}

# 3. Factory reset proof
$factoryResetResp = Invoke-Request -Method POST -Path "/api/system/factory-reset" -Headers $goodHeaders
$factoryResetSamples = Collect-StatusWindow -Name "factory-reset" -DurationSec 120
$postResetStatus = Wait-ForHealthy -TimeoutSec 120
$postResetConfig = Parse-JsonSafe((Invoke-Request -Method GET -Path "/api/config").body)

$result["factory_reset"] = [ordered]@{
  trigger = $factoryResetResp
  summary = Summarize-Window $factoryResetSamples
  post_reset_status = $postResetStatus
  post_reset_config = $postResetConfig
}

if (-not (Any-ReachableSample $factoryResetSamples) -or ($null -eq $postResetStatus)) {
  $result["manual_action_required"] = [ordered]@{
    phase = "factory_reset"
    reason = "Factory reset moved the device out of LAN reachability. Rejoin the setup AP from a phone, reprovision Wi-Fi, then rerun restore separately."
  }
  Save-JsonFile -Path (Join-Path $OutDir "destructive-proof-summary.json") -Value $result
  $result | ConvertTo-Json -Depth 20
  return
}

# Restore from protected backup in two steps so enrollment-token change does not
# wipe device_id/device_token on the same write.
$restorePayload1 = $baselineBackup | ConvertTo-Json -Depth 20 | ConvertFrom-Json
$restorePayload1 | Add-Member -NotePropertyName "admin_password" -NotePropertyValue $RestoreAdminPassword -Force
$restoreStep1Body = $restorePayload1 | ConvertTo-Json -Depth 20 -Compress
$restoreStep1Resp = Invoke-Request -Method POST -Path "/api/config/save" -Headers @{} -Body $restoreStep1Body
Start-Sleep -Seconds 2

$restoreAuthHeaders = @{ "X-Rebooter-Auth" = $RestoreAdminPassword }
$restorePayload2 = $baselineBackup | ConvertTo-Json -Depth 20 -Compress
$restoreStep2Resp = Invoke-Request -Method POST -Path "/api/config/save" -Headers $restoreAuthHeaders -Body $restorePayload2
Start-Sleep -Seconds 2

$restoreRebootResp = Invoke-Request -Method POST -Path "/api/system/reboot" -Headers $restoreAuthHeaders
$restoreSamples = Collect-StatusWindow -Name "post-restore-reboot" -DurationSec 120
$restoreFinal = Wait-ForHealthy -TimeoutSec 120
$restoreFinalConfig = Parse-JsonSafe((Invoke-Request -Method GET -Path "/api/config").body)
$restoreEvents = Get-Events

$result["restore"] = [ordered]@{
  step1 = $restoreStep1Resp
  step2 = $restoreStep2Resp
  reboot = $restoreRebootResp
  summary = Summarize-Window $restoreSamples
  final_status = $restoreFinal
  final_public_config = $restoreFinalConfig
  final_event_count = $restoreEvents.Count
}

$result["summary"] = [ordered]@{
  reboot_passed = ($result.reboot.final_status.health_state -eq "healthy" -and -not $result.reboot.final_status.recovery_mode)
  recovery_boot_passed = ($result.recovery_boot.summary.saw_recovery_mode -and $result.recovery_boot.final_status.recovery_mode)
  recovery_exit_passed = ($result.recovery_exit.final_status.health_state -eq "healthy" -and -not $result.recovery_exit.final_status.recovery_mode)
  factory_reset_returned = ($null -ne $result.factory_reset.post_reset_status)
  restore_passed = ($result.restore.final_status.health_state -eq "healthy" -and $result.restore.final_status.auth_required -and $result.restore.final_status.central_enabled)
}

Save-JsonFile -Path (Join-Path $OutDir "destructive-proof-summary.json") -Value $result
$result | ConvertTo-Json -Depth 20
