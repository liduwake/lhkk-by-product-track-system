<# =============================================================================
   File      : smartkitchen_to_thingsboard_20261101_v2.ps1
   Purpose   : Reads daily food-waste totals from the SmartKitchen API and sends
               them as daily telemetry to the ThingsBoard device SK-01 (display only, D6).
   Version   : v2 (2026-11-01)
   Changes   : v1 got HTTP 500 from ThingsBoard (JSON array from ConvertTo-Json in
               PowerShell 5.1). v2 writes the JSON itself and sends one day per request.
   AI usage  : Written with AI assistance (Claude); run and checked by the author.
   Secrets   : tokens are in secrets.ps1 (same folder). Never show or commit it.
   Use       : .\smartkitchen_to_thingsboard_20261101_v2.ps1 -Start 2026-09-07 -End 2026-10-02
               (dry run, prints only)   add -Upload to send to ThingsBoard.
               No -Start/-End = the last 4 weeks (API default).
   ============================================================================= #>
param(
  [string]$Start = "",
  [string]$End   = "",
  [switch]$Upload
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---- secrets: $SK_TOKEN, $TB_HOST, $SK01_TOKEN --------------------------------
$secretsFile = Join-Path $PSScriptRoot "secrets.ps1"
if (-not (Test-Path $secretsFile)) { throw "secrets.ps1 not found. Copy secrets.example.ps1 to secrets.ps1 and fill it in." }
. $secretsFile
if (-not $TB_HOST) { $TB_HOST = "thingsboard.cloud" }

# ---- 1. read the SmartKitchen API ---------------------------------------------
$skUrl = "https://my.smartkitchen.solutions/api/lhkk/get_food_waste.php"
$body = "{}"
if ($Start -and $End) {
  $body = '{"time":{"start":"' + $Start + '","end":"' + $End + '"}}'
}
$resp = Invoke-WebRequest -Uri $skUrl -Method Post -UseBasicParsing `
          -Headers @{ "permanent-token" = $SK_TOKEN } `
          -ContentType "application/json" -Body $body
# decode as UTF-8 so that Finnish letters in the names stay correct
$text = [Text.Encoding]::UTF8.GetString($resp.RawContentStream.ToArray())
$data = $text | ConvertFrom-Json
if ($data -is [array]) { $locations = $data } else { $locations = @($data) }

# category name -> telemetry key, e.g. "Opetuskeittio" -> "opetuskeittio_g"
function To-Key([string]$name) {
  $s = $name.ToLowerInvariant().Normalize([Text.NormalizationForm]::FormD)
  $s = $s -replace '\p{Mn}', ''            # remove the dots of the Finnish letters
  $s = ($s -replace '[^a-z0-9]+', '_').Trim('_')
  return "$($s)_g"
}

# ---- 2. sum per day ---------------------------------------------------------------
$days = @{}      # date -> hashtable of values
$catKeys = @{}
foreach ($loc in $locations) {
  foreach ($cat in $loc.categories) {
    $key = To-Key $cat.category_name
    $catKeys[$key] = $cat.category_name
    foreach ($item in $cat.items) {
      foreach ($d in $item.data) {
        if (-not $days.ContainsKey($d.date)) {
          $days[$d.date] = @{ daily_total_g = 0.0; entries = 0; ewc_code_missing = 0 }
        }
        $v = $days[$d.date]
        $v.daily_total_g += [double]$d.weight
        $v.entries += 1
        if ([string]::IsNullOrWhiteSpace([string]$d.code)) { $v.ewc_code_missing += 1 }
        if (-not $v.ContainsKey($key)) { $v[$key] = 0.0 }
        $v[$key] += [double]$d.weight
      }
    }
  }
}
if ($days.Count -eq 0) { Write-Host "No data returned for this period."; exit }

# ---- 3. build the telemetry (one entry per day, time stamp 12:00 UTC of that day)
$list = @()
$sumG = 0.0; $sumEntries = 0; $sumMissing = 0
Write-Host ""
Write-Host "date         total_kg  entries  ewc_missing"
foreach ($date in ($days.Keys | Sort-Object)) {
  $v = $days[$date]
  $dt = [datetime]::ParseExact($date, "yyyy-MM-dd", $null)
  $ts = ([DateTimeOffset]::new($dt.Year, $dt.Month, $dt.Day, 12, 0, 0, [TimeSpan]::Zero)).ToUnixTimeMilliseconds()
  $pairs = @()
  foreach ($k in ($v.Keys | Sort-Object)) {
    $n = [int64][math]::Round([double]$v[$k])           # all values are whole numbers
    $pairs += ('"{0}":{1}' -f $k, $n)
  }
  # same body format as the firmware: {"ts":...,"values":{...}}
  $list += ('{{"ts":{0},"values":{{{1}}}}}' -f $ts, ($pairs -join ','))
  $sumG += $v.daily_total_g; $sumEntries += $v.entries; $sumMissing += $v.ewc_code_missing
  "{0}  {1,8:N1}  {2,7}  {3,11}" -f $date, ($v.daily_total_g / 1000), $v.entries, $v.ewc_code_missing | Write-Host
}
Write-Host ""
"Days: {0}   entries: {1}   total: {2:N1} kg   EWC code empty: {3} of {1}" -f $days.Count, $sumEntries, ($sumG / 1000), $sumMissing | Write-Host
Write-Host "Category keys:"
foreach ($k in ($catKeys.Keys | Sort-Object)) { Write-Host ("  {0}  ({1})" -f $k, $catKeys[$k]) }

# ---- 4. send to ThingsBoard (only with -Upload) ----------------------------------
if (-not $Upload) { Write-Host ""; Write-Host "Dry run: nothing was sent. Add -Upload to send."; exit }
$tbUrl = "https://$TB_HOST/api/v1/$SK01_TOKEN/telemetry"
$ok = 0
Write-Host ""
foreach ($body in $list) {
  try {
    $r = Invoke-WebRequest -Uri $tbUrl -Method Post -UseBasicParsing `
           -ContentType "application/json" -Body ([Text.Encoding]::ASCII.GetBytes($body))
    $ok++
  } catch {
    $code = $_.Exception.Response.StatusCode.value__
    Write-Host ("Send failed (HTTP {0}) for: {1}" -f $code, $body)
    Write-Host "Stopped. Nothing else was sent."
    exit 1
  }
}
Write-Host ("ThingsBoard answered HTTP 200 for all {0} days. Data sent to SK-01." -f $ok)
