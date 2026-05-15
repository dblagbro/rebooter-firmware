param(
  [string]$BaseUrl = "http://192.168.1.48",
  [string]$AuthToken = "",
  [string]$ResultsPath = ""
)

$ErrorActionPreference = "Stop"

function Invoke-Api {
  param(
    [string]$Method,
    [string]$Path,
    [hashtable]$Headers = @{},
    [string]$Body = "",
    [string]$ContentType = "application/json"
  )

  $uri = "$BaseUrl$Path"
  try {
    $params = @{
      Uri = $uri
      Method = $Method
      Headers = $Headers
      UseBasicParsing = $true
    }
    if ($Body) {
      $params.Body = $Body
      $params.ContentType = $ContentType
    }
    $response = Invoke-WebRequest @params
    return [ordered]@{
      ok = $true
      status = [int]$response.StatusCode
      body = [string]$response.Content
    }
  } catch {
    $resp = $_.Exception.Response
    if ($resp) {
      $reader = New-Object System.IO.StreamReader($resp.GetResponseStream())
      $content = $reader.ReadToEnd()
      $reader.Close()
      return [ordered]@{
        ok = $false
        status = [int]$resp.StatusCode
        body = [string]$content
      }
    }
    throw
  }
}

function Parse-JsonBody {
  param([string]$Body)
  if ([string]::IsNullOrWhiteSpace($Body)) { return $null }
  try {
    return $Body | ConvertFrom-Json
  } catch {
    return $null
  }
}

function Add-Assertion {
  param(
    [System.Collections.Generic.List[object]]$Assertions,
    [string]$Name,
    [bool]$Passed,
    [string]$Details
  )
  $Assertions.Add([ordered]@{
    name = $Name
    passed = $Passed
    details = $Details
  })
}

$results = [ordered]@{
  date = (Get-Date).ToString("s")
  base_url = $BaseUrl
  assertions = New-Object 'System.Collections.Generic.List[object]'
}

$statusResp = Invoke-Api -Method "GET" -Path "/api/status"
$statusJson = Parse-JsonBody $statusResp.body
Add-Assertion $results.assertions "status endpoint reachable" ($statusResp.status -eq 200 -and $null -ne $statusJson) "status=$($statusResp.status)"
Add-Assertion $results.assertions "status exposes auth_required" ($null -ne $statusJson.auth_required) "auth_required=$($statusJson.auth_required)"

$configResp = Invoke-Api -Method "GET" -Path "/api/config"
$configJson = Parse-JsonBody $configResp.body
$publicConfigBody = [string]$configResp.body
Add-Assertion $results.assertions "public config reachable" ($configResp.status -eq 200 -and $null -ne $configJson) "status=$($configResp.status)"
Add-Assertion $results.assertions "public config redacts enrollment token" ($publicConfigBody -notmatch '"enrollment_token"\s*:') "checked for enrollment_token"
Add-Assertion $results.assertions "public config redacts device token" ($publicConfigBody -notmatch '"device_token"\s*:') "checked for device_token"
Add-Assertion $results.assertions "public config redacts site id" ($publicConfigBody -notmatch '"site_id"\s*:') "checked for site_id"
Add-Assertion $results.assertions "public config redacts device id" ($publicConfigBody -notmatch '"device_id"\s*:') "checked for device_id"

$eventsResp = Invoke-Api -Method "GET" -Path "/api/events"
$eventsJson = Parse-JsonBody $eventsResp.body
$eventItems = @()
if ($eventsJson -is [System.Array]) {
  $eventItems = @($eventsJson)
} elseif ($eventsJson -and $eventsJson.events) {
  $eventItems = @($eventsJson.events)
}
$firstEvent = if ($eventItems.Count -gt 0) { $eventItems[0] } else { $null }
$hasSequencedEvent = $false
foreach ($event in $eventItems) {
  if (($null -ne $event.seq) -and ($null -ne $event.boot_id) -and ($event.ts_basis -eq "uptime_seconds")) {
    $hasSequencedEvent = $true
    break
  }
}
Add-Assertion $results.assertions "events endpoint reachable" ($eventsResp.status -eq 200 -and $eventItems.Count -gt 0) "status=$($eventsResp.status); count=$($eventItems.Count)"
Add-Assertion $results.assertions "events expose seq" ($null -ne $firstEvent -and $null -ne $firstEvent.seq) "first_event_seq=$($firstEvent.seq)"
Add-Assertion $results.assertions "events expose boot_id" ($null -ne $firstEvent -and $null -ne $firstEvent.boot_id) "first_event_boot_id=$($firstEvent.boot_id)"
Add-Assertion $results.assertions "events expose ts_basis" $hasSequencedEvent "first_event_ts_basis=$($firstEvent.ts_basis)"

$relayMethodResp = Invoke-Api -Method "GET" -Path "/api/relay/on"
Add-Assertion $results.assertions "wrong method returns 405 for relay on" ($relayMethodResp.status -eq 405) "status=$($relayMethodResp.status)"

$faviconResp = Invoke-Api -Method "GET" -Path "/favicon.ico"
Add-Assertion $results.assertions "favicon no longer 404s" (($faviconResp.status -eq 204) -or ($faviconResp.status -eq 200)) "status=$($faviconResp.status)"

$authRequired = [bool]$statusJson.auth_required
if ($authRequired) {
  $backupUnauthResp = Invoke-Api -Method "GET" -Path "/api/system/config-backup"
  Add-Assertion $results.assertions "protected backup rejects unauthenticated access" ($backupUnauthResp.status -eq 401) "status=$($backupUnauthResp.status)"
} else {
  Add-Assertion $results.assertions "protected backup unauth rejection skipped" $true "auth not required on device"
}

if (-not [string]::IsNullOrWhiteSpace($AuthToken)) {
  $authHeaders = @{ "X-Rebooter-Auth" = $AuthToken }

  $backupAuthResp = Invoke-Api -Method "GET" -Path "/api/system/config-backup" -Headers $authHeaders
  $heartbeatResp = Invoke-Api -Method "GET" -Path "/api/system/heartbeat-preview" -Headers $authHeaders
  $diagnosticResp = Invoke-Api -Method "GET" -Path "/api/system/central-diagnostic" -Headers $authHeaders

  $backupBody = [string]$backupAuthResp.body
  $heartbeatJson = Parse-JsonBody $heartbeatResp.body
  $diagnosticJson = Parse-JsonBody $diagnosticResp.body

  Add-Assertion $results.assertions "protected backup succeeds with auth" ($backupAuthResp.status -eq 200) "status=$($backupAuthResp.status)"
  Add-Assertion $results.assertions "protected backup still includes enrollment token" ($backupBody -match '"enrollment_token"\s*:') "checked for enrollment_token"
  Add-Assertion $results.assertions "protected backup still includes device token" ($backupBody -match '"device_token"\s*:') "checked for device_token"
  Add-Assertion $results.assertions "heartbeat preview succeeds with auth" ($heartbeatResp.status -eq 200 -and $null -ne $heartbeatJson) "status=$($heartbeatResp.status)"
  Add-Assertion $results.assertions "heartbeat preview includes reported_config" ($null -ne $heartbeatJson.reported_config) "reported_config_present=$($null -ne $heartbeatJson.reported_config)"

  $allTargetsHealthy = $true
  $noLegacySecondary = $true
  if ($diagnosticJson -and $diagnosticJson.targets) {
    foreach ($target in $diagnosticJson.targets) {
      if ($target.version_url -match "www2\\.voipguru\\.org") {
        $noLegacySecondary = $false
      }
      if ($null -ne $target.https_code -and [int]$target.https_code -ge 400) {
        $allTargetsHealthy = $false
      }
    }
  }
  Add-Assertion $results.assertions "central diagnostic omits legacy secondary" $noLegacySecondary "legacy_secondary_present=$(-not $noLegacySecondary)"
  Add-Assertion $results.assertions "central diagnostic has no 4xx/5xx targets" $allTargetsHealthy "see diagnostic payload"
} else {
  Add-Assertion $results.assertions "protected auth-path checks skipped" $true "no auth token supplied"
}

$results.summary = [ordered]@{
  total = $results.assertions.Count
  passed = @($results.assertions | Where-Object { $_.passed }).Count
  failed = @($results.assertions | Where-Object { -not $_.passed }).Count
}

$json = $results | ConvertTo-Json -Depth 8
if ($ResultsPath) {
  Set-Content -Path $ResultsPath -Value $json
}
$json
