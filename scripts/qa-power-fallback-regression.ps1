param(
  [string]$BaseUrl = "http://192.168.1.48",
  [string]$AuthToken = "",
  [string]$BadBaseUrl = "https://www2.voipguru.org/rebooter",
  [string]$GoodBaseUrl = "https://www.voipguru.org/rebooter",
  [int]$WatchSeconds = 30,
  [string]$ResultsPath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($AuthToken)) {
  throw "AuthToken is required."
}

function Invoke-Json {
  param(
    [string]$Method,
    [string]$Path,
    [hashtable]$Headers = @{},
    [object]$Body = $null
  )

  $uri = "$BaseUrl$Path"
  $params = @{
    Uri = $uri
    Method = $Method
    Headers = $Headers
    TimeoutSec = 10
  }

  if ($null -ne $Body) {
    $params.ContentType = "application/json"
    $params.Body = ($Body | ConvertTo-Json -Depth 20)
  }

  Invoke-RestMethod @params
}

function Get-EventItems {
  $response = Invoke-WebRequest -Uri "$BaseUrl/api/events" -Method GET -TimeoutSec 10 -UseBasicParsing
  $parsed = $response.Content | ConvertFrom-Json
  if ($parsed -is [System.Array]) { return @($parsed) }
  if ($null -ne $parsed.value) { return @($parsed.value) }
  if ($null -ne $parsed.seq) { return @($parsed) }
  return @()
}

$authHeaders = @{ "X-Rebooter-Auth" = $AuthToken }
$originalConfig = Invoke-Json -Method "GET" -Path "/api/config" -Headers $authHeaders
$baselineEvents = Get-EventItems
$baselineSeq = 0
if ($baselineEvents.Count -gt 0) {
  $seqEvents = @($baselineEvents | Where-Object { $null -ne $_.seq })
  if ($seqEvents.Count -gt 0) {
    $baselineSeq = ($seqEvents | Measure-Object -Property seq -Maximum).Maximum
  }
}

$results = [ordered]@{
  date = (Get-Date).ToString("s")
  base_url = $BaseUrl
  bad_base_url = $BadBaseUrl
  good_base_url = $GoodBaseUrl
  baseline_seq = $baselineSeq
  samples = New-Object 'System.Collections.Generic.List[object]'
  assertions = New-Object 'System.Collections.Generic.List[object]'
}

try {
  $testConfig = $originalConfig | ConvertTo-Json -Depth 20 | ConvertFrom-Json
  $testConfig.power.enabled = $true
  $testConfig.power.sample_rate_hz = 1
  $testConfig.power.batch_seconds = 10
  $testConfig.central.base_urls = @($BadBaseUrl, $GoodBaseUrl)
  Invoke-Json -Method "POST" -Path "/api/config/save" -Headers $authHeaders -Body $testConfig | Out-Null

  $deadline = (Get-Date).AddSeconds($WatchSeconds)
  while ((Get-Date) -lt $deadline) {
    $status = Invoke-Json -Method "GET" -Path "/api/status"
    $events = Get-EventItems
    $recent = @($events | Where-Object { $_.seq -gt $baselineSeq })
    $results.samples.Add([ordered]@{
      sampled_at = (Get-Date).ToString("o")
      uptime_seconds = $status.uptime_seconds
      central_state = $status.central_state
      recent = @($recent | Select-Object seq, boot_id, ts, type, message)
    })
    Start-Sleep -Seconds 4
  }

  $allMessages = @()
  foreach ($sample in $results.samples) {
    foreach ($event in $sample.recent) {
      $allMessages += [string]$event.message
    }
  }

  $sawGoodUpload = $allMessages | Where-Object { $_ -like "*Power-sample batch uploaded via $GoodBaseUrl*" } | Select-Object -First 1
  $sawPower404 = $allMessages | Where-Object { $_ -like "*Power-sample upload failed (404)*" } | Select-Object -First 1
  $sawFirmware404 = $allMessages | Where-Object { $_ -like "*Firmware assignment check failed (404)*" } | Select-Object -First 1
  $sawBadState = $results.samples | Where-Object { $_.central_state -eq "firmware_check_failed" } | Select-Object -First 1

  $results.assertions.Add([ordered]@{
    name = "power upload falls through to good base url"
    passed = ($null -ne $sawGoodUpload)
    details = if ($null -ne $sawGoodUpload) { [string]$sawGoodUpload } else { "no upload observed via good base url" }
  })
  $results.assertions.Add([ordered]@{
    name = "power upload avoids 404 failure on bad base url"
    passed = ($null -eq $sawPower404)
    details = if ($null -eq $sawPower404) { "no power upload 404 observed" } else { [string]$sawPower404 }
  })
  $results.assertions.Add([ordered]@{
    name = "firmware assignment avoids 404 failure on bad base url"
    passed = ($null -eq $sawFirmware404)
    details = if ($null -eq $sawFirmware404) { "no firmware assignment 404 observed" } else { [string]$sawFirmware404 }
  })
  $results.assertions.Add([ordered]@{
    name = "central state stays out of firmware_check_failed"
    passed = ($null -eq $sawBadState)
    details = if ($null -eq $sawBadState) { "central state remained healthy/idle" } else { "saw firmware_check_failed" }
  })
}
finally {
  try {
    Invoke-Json -Method "POST" -Path "/api/config/save" -Headers $authHeaders -Body $originalConfig | Out-Null
  } catch {
    $results.restore_error = $_.Exception.Message
  }
}

$passedCount = @($results.assertions | Where-Object { $_.passed }).Count
$results.summary = [ordered]@{
  total = $results.assertions.Count
  passed = $passedCount
  failed = $results.assertions.Count - $passedCount
}

$json = $results | ConvertTo-Json -Depth 12
if (-not [string]::IsNullOrWhiteSpace($ResultsPath)) {
  Set-Content -Path $ResultsPath -Value $json
}
$json
