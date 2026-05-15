param(
  [string]$BaseUrl = "http://192.168.1.48",
  [string]$GoodAuthToken = "",
  [string]$BadAuthToken = "totally-wrong-token",
  [string]$ResultsPath = ""
)

$ErrorActionPreference = "Stop"

function Invoke-Check {
  param(
    [string]$Method,
    [string]$Path,
    [hashtable]$Headers = @{},
    [string]$Body = $null,
    [string]$ContentType = "application/json"
  )

  try {
    $params = @{
      Uri = "$BaseUrl$Path"
      Method = $Method
      Headers = $Headers
      UseBasicParsing = $true
      TimeoutSec = 15
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
    throw
  }
}

function Convert-JsonSafe {
  param([string]$Body)
  if ([string]::IsNullOrWhiteSpace($Body)) { return $null }
  try { return $Body | ConvertFrom-Json } catch { return $null }
}

$goodHeaders = @{}
if (-not [string]::IsNullOrWhiteSpace($GoodAuthToken)) {
  $goodHeaders["X-Rebooter-Auth"] = $GoodAuthToken
}
$badHeaders = @{}
if (-not [string]::IsNullOrWhiteSpace($BadAuthToken)) {
  $badHeaders["X-Rebooter-Auth"] = $BadAuthToken
}

$checks = New-Object System.Collections.Generic.List[object]
function Add-Check {
  param([string]$Name, [bool]$Passed, [string]$Details)
  $checks.Add([pscustomobject]@{
    name = $Name
    passed = $Passed
    details = $Details
  })
}

$status = Convert-JsonSafe((Invoke-Check GET "/api/status").body)
$config = Convert-JsonSafe((Invoke-Check GET "/api/config").body)
$backup = Convert-JsonSafe((Invoke-Check GET "/api/system/config-backup" $goodHeaders).body)
$diag = Convert-JsonSafe((Invoke-Check GET "/api/system/central-diagnostic" $goodHeaders).body)
$events = Convert-JsonSafe((Invoke-Check GET "/api/events").body)

Add-Check "status healthy and normal mode" (($status.health_state -eq "healthy") -and (-not $status.recovery_mode) -and ($status.central_state -eq "idle")) "health=$($status.health_state); recovery=$($status.recovery_mode); central=$($status.central_state)"
Add-Check "status requires auth" ($status.auth_required -eq $true) "auth_required=$($status.auth_required)"

$centralProps = @($config.central.PSObject.Properties.Name)
Add-Check "public config keeps safe central summary" (($config.central.registered -eq $true) -and ($null -ne $config.central.has_enrollment_token)) "registered=$($config.central.registered); has_enrollment_token=$($config.central.has_enrollment_token)"
Add-Check "public config excludes secrets deeply" (($centralProps -notcontains "enrollment_token") -and ($centralProps -notcontains "device_token") -and ($centralProps -notcontains "site_id") -and ($centralProps -notcontains "device_id")) ($centralProps -join ",")
Add-Check "protected backup includes secrets" (($backup.central.enrollment_token.Length -gt 0) -and ($backup.central.device_token.Length -gt 0) -and ($backup.central.device_id.Length -gt 0)) "device_id_len=$($backup.central.device_id.Length); token_len=$($backup.central.device_token.Length)"

foreach ($path in @("/api/system/config-backup", "/api/system/heartbeat-preview", "/api/system/central-diagnostic")) {
  $unauth = Invoke-Check GET $path
  $wrong = Invoke-Check GET $path $badHeaders
  Add-Check "unauth rejected $path" ($unauth.status -eq 401) "status=$($unauth.status)"
  Add-Check "wrong token rejected $path" ($wrong.status -eq 401) "status=$($wrong.status)"
}

$missingBody = Invoke-Check POST "/api/config/save" $goodHeaders
Add-Check "config save missing body returns 400" ($missingBody.status -eq 400) "status=$($missingBody.status)"
$invalidJson = Invoke-Check POST "/api/config/save" $goodHeaders "{" 
Add-Check "config save invalid json returns 400" ($invalidJson.status -eq 400) "status=$($invalidJson.status)"
$saveWrong = Invoke-Check POST "/api/config/save" $badHeaders "{}"
Add-Check "config save wrong token returns 401" ($saveWrong.status -eq 401) "status=$($saveWrong.status)"
$relayWrong = Invoke-Check POST "/api/relay/on" $badHeaders
Add-Check "relay wrong token returns 401" ($relayWrong.status -eq 401) "status=$($relayWrong.status)"

$methodPaths = @(
  "/api/relay/on",
  "/api/relay/off",
  "/api/relay/toggle",
  "/api/config/save",
  "/api/system/reboot",
  "/api/system/recovery-boot",
  "/api/system/factory-reset",
  "/api/system/ota"
)
foreach ($path in $methodPaths) {
  $resp = Invoke-Check GET $path
  Add-Check "405 $path" ($resp.status -eq 405) "status=$($resp.status)"
}

$diagTargets = @($diag.targets)
Add-Check "central diagnostic has exactly one default target" ($diagTargets.Count -eq 1) "count=$($diagTargets.Count)"
if ($diagTargets.Count -gt 0) {
  Add-Check "central diagnostic target healthy" (($diagTargets[0].https_code -eq 200) -and ($diagTargets[0].version_url -notmatch "www2")) "code=$($diagTargets[0].https_code); url=$($diagTargets[0].version_url)"
}

$eventList = @($events)
$seqMonotonic = $true
$prevSeq = -1
$bootIds = @()
$nonUptimeBasis = 0
foreach ($evt in $eventList) {
  if ([int]$evt.seq -le $prevSeq) { $seqMonotonic = $false }
  $prevSeq = [int]$evt.seq
  $bootIds += [int]$evt.boot_id
  if ($evt.ts_basis -ne "uptime_seconds") { $nonUptimeBasis += 1 }
}
Add-Check "events are sequenced monotonically" $seqMonotonic ("seqs=" + (($eventList | ForEach-Object seq) -join ","))
Add-Check "events include multiple boot ids" (((@($bootIds | Select-Object -Unique)).Count) -ge 2) ("boot_ids=" + ((@($bootIds | Select-Object -Unique)) -join ","))
Add-Check "events all expose ts_basis" ($nonUptimeBasis -eq 0) "non_uptime_basis_count=$nonUptimeBasis"

$originalMonitor = [int]$config.monitor_interval_seconds
$tempMonitor = if ($originalMonitor -eq 5) { 6 } else { 5 }
$saveBody = @{ monitor_interval_seconds = $tempMonitor } | ConvertTo-Json -Compress
$saveResp = Invoke-Check POST "/api/config/save" $goodHeaders $saveBody
Start-Sleep -Seconds 1
$configAfterSave = Convert-JsonSafe((Invoke-Check GET "/api/config").body)
$revertBody = @{ monitor_interval_seconds = $originalMonitor } | ConvertTo-Json -Compress
$revertResp = Invoke-Check POST "/api/config/save" $goodHeaders $revertBody
Start-Sleep -Seconds 1
$configAfterRevert = Convert-JsonSafe((Invoke-Check GET "/api/config").body)

Add-Check "config save accepted with auth" ($saveResp.status -eq 200) "status=$($saveResp.status)"
Add-Check "config save persisted temp monitor interval" ([int]$configAfterSave.monitor_interval_seconds -eq $tempMonitor) "saved=$($configAfterSave.monitor_interval_seconds); expected=$tempMonitor"
Add-Check "config save revert accepted with auth" ($revertResp.status -eq 200) "status=$($revertResp.status)"
Add-Check "config save reverted monitor interval" ([int]$configAfterRevert.monitor_interval_seconds -eq $originalMonitor) "reverted=$($configAfterRevert.monitor_interval_seconds); expected=$originalMonitor"
Add-Check "central base_urls persist without legacy secondary after save cycle" ((@($configAfterRevert.central.base_urls)).Count -eq 1 -and $configAfterRevert.central.base_urls[0] -notmatch "www2") (($configAfterRevert.central.base_urls) -join ",")

$result = [pscustomobject]@{
  date = (Get-Date).ToString("s")
  base_url = $BaseUrl
  checks = $checks
  summary = [pscustomobject]@{
    total = $checks.Count
    passed = @($checks | Where-Object passed).Count
    failed = @($checks | Where-Object { -not $_.passed }).Count
  }
}

$json = $result | ConvertTo-Json -Depth 8
if ($ResultsPath) {
  Set-Content -Path $ResultsPath -Value $json
}
$json
