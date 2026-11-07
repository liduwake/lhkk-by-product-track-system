<# =============================================================================
   File      : simulated_events_to_thingsboard_20261103_v2.ps1
   Purpose   : Sends the SIMULATED example events (default simulated_events_20261103_v2.csv) of the
               planned measuring points MP-01..MP-04 to ThingsBoard, one event per request,
               in the same format as the firmware. Estimates, not measurements.
   Version   : v2 (2026-11-03)   Changes: CSV file is a parameter (-Csv), default v2 file
   AI usage  : Written by Claude (AI); run and checked by the author.
   Secrets   : device tokens are in secrets.ps1 (same folder). Never show or commit it.
   Use       : .\simulated_events_to_thingsboard_20261103_v2.ps1            (dry run)
               .\simulated_events_to_thingsboard_20261103_v2.ps1 -Upload    (send all)
               add -Only MP-03 to send one measuring point only
   ============================================================================= #>
param(
  [switch]$Upload,
  [string]$Only = "",
  [string]$Csv = "simulated_events_20261103_v2.csv"
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$secretsFile = Join-Path $PSScriptRoot "secrets.ps1"
if (-not (Test-Path $secretsFile)) { throw "secrets.ps1 not found. Copy secrets.example.ps1 to secrets.ps1 and fill it in." }
. $secretsFile
if (-not $TB_HOST) { $TB_HOST = "thingsboard.cloud" }

$rows = Import-Csv (Join-Path $PSScriptRoot $Csv)
if ($Only) { $rows = @($rows | Where-Object { $_.measuring_point_id -eq $Only }) }

# ---- dry-run summary --------------------------------------------------------------
Write-Host ""
Write-Host "point   events  added_kg  removed_kg  emptied_kg  first day   last day    token"
foreach ($g in ($rows | Group-Object measuring_point_id | Sort-Object Name)) {
  $add = 0.0; $rem = 0.0; $emp = 0.0
  foreach ($r in $g.Group) {
    $w = [double]$r.weight_change_g
    if ($r.event_type -eq "addition") { $add += $w }
    elseif ($r.event_type -eq "removal") { $rem += $w }
    elseif ($r.event_type -eq "emptying") { $emp += $w }
  }
  $hasToken = if ($TB_TOKENS.ContainsKey($g.Name) -and $TB_TOKENS[$g.Name] -notlike "YOUR_*") { "ok" } else { "MISSING" }
  "{0,-7} {1,6}  {2,8:N1}  {3,10:N1}  {4,10:N1}  {5}  {6}  {7}" -f $g.Name, $g.Count, ($add/1000), ($rem/1000), ($emp/1000),
     $g.Group[0].measured_at.Substring(0,10), $g.Group[-1].measured_at.Substring(0,10), $hasToken | Write-Host
}
if (-not $Upload) { Write-Host ""; Write-Host "Dry run: nothing was sent. Add -Upload to send."; exit }

# ---- upload: one event per request, short pause (ThingsBoard rate limits) ----------
$sent = 0
foreach ($r in $rows) {
  $token = $TB_TOKENS[$r.measuring_point_id]
  if (-not $token -or $token -like "YOUR_*") { Write-Host ("No token for {0}. Stopped." -f $r.measuring_point_id); exit 1 }
  $ts = [DateTimeOffset]::Parse($r.measured_at, [Globalization.CultureInfo]::InvariantCulture).ToUnixTimeMilliseconds()
  $body = '{{"ts":{0},"values":{{"measuring_point_id":"{1}","event_type":"{2}","weight_change":{3},"content_after":{4}}}}}' -f `
          $ts, $r.measuring_point_id, $r.event_type, [int64]$r.weight_change_g, [int64]$r.content_after_g
  try {
    $resp = Invoke-WebRequest -Uri "https://$TB_HOST/api/v1/$token/telemetry" -Method Post -UseBasicParsing `
              -ContentType "application/json" -Body ([Text.Encoding]::ASCII.GetBytes($body))
  } catch {
    $code = $_.Exception.Response.StatusCode.value__
    Write-Host ("Send failed (HTTP {0}) at {1} {2}. Stopped after {3} events." -f $code, $r.measuring_point_id, $r.measured_at, $sent)
    exit 1
  }
  $sent++
  if ($sent % 20 -eq 0) { Write-Host ("  {0} of {1} sent" -f $sent, $rows.Count) }
  Start-Sleep -Milliseconds 300
}
Write-Host ("ThingsBoard answered HTTP 200 for all {0} events." -f $sent)
