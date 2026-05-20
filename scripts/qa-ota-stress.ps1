param(
  [string]$BaseUrl = "http://192.168.1.48",
  [string]$AuthToken = "",
  [string]$FirmwarePath = "C:\dev\rebooter-firmware\.pio\build\sonoff_s31\firmware.bin",
  [int]$Cycles = 3,
  [int]$PollSeconds = 90,
  [string]$ResultsPath = ""
)

$ErrorActionPreference = "Stop"

function Invoke-Status {
  try {
    $resp = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/api/status" -TimeoutSec 5
    return $resp.Content | ConvertFrom-Json
  } catch {
    return $null
  }
}

function Invoke-Events {
  try {
    $resp = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/api/events" -TimeoutSec 10
    return $resp.Content | ConvertFrom-Json
  } catch {
    return @()
  }
}

function Invoke-AuthPost {
  param([string]$Path)
  try {
    $headers = @{}
    if (-not [string]::IsNullOrWhiteSpace($AuthToken)) {
      $headers["X-Rebooter-Auth"] = $AuthToken
    }
    $resp = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl$Path" -Method POST -Headers $headers -TimeoutSec 15
    return [pscustomobject]@{ status = [int]$resp.StatusCode; body = [string]$resp.Content }
  } catch {
    $resp = $_.Exception.Response
    if ($resp) {
      $reader = New-Object System.IO.StreamReader($resp.GetResponseStream())
      $content = $reader.ReadToEnd()
      $reader.Close()
      return [pscustomobject]@{ status = [int]$resp.StatusCode; body = [string]$content }
    }
    return [pscustomobject]@{ status = -1; body = $_.Exception.Message }
  }
}

function Invoke-OtaUpload {
  Add-Type -AssemblyName System.Net.Http
  $bytes = [System.IO.File]::ReadAllBytes($FirmwarePath)
  $client = [System.Net.Http.HttpClient]::new()
  $content = [System.Net.Http.MultipartFormDataContent]::new()
  $fileContent = [System.Net.Http.ByteArrayContent]::new($bytes)
  $fileContent.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse("application/octet-stream")
  $content.Add($fileContent, "update", [System.IO.Path]::GetFileName($FirmwarePath))

  $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, "$BaseUrl/api/system/ota")
  if (-not [string]::IsNullOrWhiteSpace($AuthToken)) {
    $request.Headers.Add("X-Rebooter-Auth", $AuthToken)
  }
  $request.Content = $content

  try {
    $response = $client.SendAsync($request).GetAwaiter().GetResult()
    return [pscustomobject]@{
      accepted = ($response.IsSuccessStatusCode -and [int]$response.StatusCode -eq 200)
      status = [int]$response.StatusCode
      body = [string]$response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
    }
  } catch {
    return [pscustomobject]@{
      accepted = $false
      status = -1
      body = $_.Exception.Message
    }
  } finally {
    $client.Dispose()
    $content.Dispose()
    $fileContent.Dispose()
    $request.Dispose()
  }
}

if (-not (Test-Path $FirmwarePath)) {
  throw "Firmware file not found: $FirmwarePath"
}

$baselineStatus = Invoke-Status
$baselineEventsRaw = Invoke-Events
$baselineEvents = if ($baselineEventsRaw -is [System.Array]) { @($baselineEventsRaw) } elseif ($null -ne $baselineEventsRaw) { @($baselineEventsRaw) } else { @() }
$lastBaselineEvent = if ($baselineEvents.Count -gt 0) { $baselineEvents[$baselineEvents.Count - 1] } else { $null }
$baselineBootId = if ($null -ne $lastBaselineEvent) { [int]$lastBaselineEvent.boot_id } else { 0 }

$results = [ordered]@{
  date = (Get-Date).ToString("s")
  base_url = $BaseUrl
  firmware_path = $FirmwarePath
  cycles = $Cycles
  baseline = [ordered]@{
    firmware_version = $baselineStatus.firmware_version
    recovery_mode = $baselineStatus.recovery_mode
    central_state = $baselineStatus.central_state
    last_boot_id = $baselineBootId
  }
  cycle_results = New-Object System.Collections.Generic.List[object]
}

$lastSeenBootId = $baselineBootId

for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
  $upload = Invoke-OtaUpload
  $uploadStdout = [string]$upload.body
  $uploadExitCode = if ($upload.accepted) { 0 } else { $upload.status }
  $accepted = $upload.accepted
  $deadline = (Get-Date).AddSeconds($PollSeconds)
  $samples = New-Object System.Collections.Generic.List[object]
  $firstReachableSeconds = $null
  $sawRecoveryMode = $false
  $sawRecoveryState = $false
  $sawLastKnownGoodRestored = $false

  while ((Get-Date) -lt $deadline) {
    $status = Invoke-Status
    if ($null -ne $status) {
      if ($null -eq $firstReachableSeconds) {
        $firstReachableSeconds = [int]([TimeSpan]((Get-Date) - ($deadline.AddSeconds(-$PollSeconds)))).TotalSeconds
      }
      if ($status.recovery_mode) { $sawRecoveryMode = $true }
      if ($status.central_state -eq "recovery_mode") { $sawRecoveryState = $true }
      if ($status.last_known_good_restored) { $sawLastKnownGoodRestored = $true }
      $samples.Add([ordered]@{
        at = (Get-Date).ToString("s")
        uptime_seconds = $status.uptime_seconds
        firmware_version = $status.firmware_version
        recovery_mode = $status.recovery_mode
        last_known_good_restored = $status.last_known_good_restored
        central_state = $status.central_state
        central_registered = $status.central_registered
        health_state = $status.health_state
      })
    }
    Start-Sleep -Seconds 2
  }

  $finalStatus = if ($samples.Count -gt 0) { $samples[$samples.Count - 1] } else { $null }
  $eventsRaw = Invoke-Events
  $events = if ($eventsRaw -is [System.Array]) { @($eventsRaw) } elseif ($null -ne $eventsRaw) { @($eventsRaw) } else { @() }
  $bootIds = @($events | ForEach-Object { [int]$_.boot_id } | Select-Object -Unique)
  $maxBootId = if ($bootIds.Count -gt 0) { ($bootIds | Measure-Object -Maximum).Maximum } else { $lastSeenBootId }
  $newBootObserved = $maxBootId -gt $lastSeenBootId
  $lastSeenBootId = $maxBootId
  $seqs = @($events | ForEach-Object { [int]$_.seq })
  $seqMonotonic = $true
  $prev = -1
  foreach ($seq in $seqs) {
    if ($seq -le $prev) { $seqMonotonic = $false; break }
    $prev = $seq
  }

  $cycleResult = [ordered]@{
    cycle = $cycle
    upload_accepted = $accepted
    upload_exit_code = $uploadExitCode
    upload_stdout = [string]$uploadStdout
    first_reachable_seconds = $firstReachableSeconds
    samples = $samples
    saw_recovery_mode = $sawRecoveryMode
    saw_recovery_state = $sawRecoveryState
    saw_last_known_good_restored = $sawLastKnownGoodRestored
    final_status = $finalStatus
    new_boot_observed = $newBootObserved
    max_boot_id = $maxBootId
    seq_monotonic = $seqMonotonic
  }

  $effectiveUploadAccepted =
    $accepted -or
    ($newBootObserved -and $null -ne $firstReachableSeconds -and $null -ne $finalStatus)
  $effectiveUploadReason =
    if ($accepted) { "http_200" }
    elseif ($effectiveUploadAccepted) { "reboot_cutoff_assumed" }
    else { "transport_or_no_reboot" }
  $cycleResult["effective_upload_accepted"] = $effectiveUploadAccepted
  $cycleResult["effective_upload_reason"] = $effectiveUploadReason

  if ($null -ne $finalStatus -and ($finalStatus.recovery_mode -or $finalStatus.central_state -eq "recovery_mode")) {
    $rebootResp = Invoke-AuthPost "/api/system/reboot"
    $cycleResult["recovery_cleanup_reboot"] = $rebootResp
    Start-Sleep -Seconds 8
  }

  $results.cycle_results.Add($cycleResult)
}

$summaryFailures = @()
foreach ($cycleResult in $results.cycle_results) {
  if (-not $cycleResult.effective_upload_accepted) { $summaryFailures += "cycle $($cycleResult.cycle): upload not accepted" }
  if ($null -eq $cycleResult.first_reachable_seconds) { $summaryFailures += "cycle $($cycleResult.cycle): device never became reachable" }
  if (-not $cycleResult.new_boot_observed) { $summaryFailures += "cycle $($cycleResult.cycle): no new boot observed in event log" }
  if (-not $cycleResult.seq_monotonic) { $summaryFailures += "cycle $($cycleResult.cycle): event seq not monotonic" }
  if ($cycleResult.saw_recovery_mode -or $cycleResult.saw_recovery_state) { $summaryFailures += "cycle $($cycleResult.cycle): recovery mode observed after OTA" }
}

$results["summary"] = [ordered]@{
  failures = $summaryFailures
  passed = ($summaryFailures.Count -eq 0)
}

$json = $results | ConvertTo-Json -Depth 8
if ($ResultsPath) {
  Set-Content -Path $ResultsPath -Value $json
}
$json
