<# =============================================================================
   File      : thingsboard_setup_devices_20261101_v2.ps1
   Purpose   : Creates the planned measuring points MP-02..MP-04 in ThingsBoard with their
               server attributes (Table 4 keys), sets the label "simulated data" (also on MP-01)
               and writes the device tokens of MP-01..MP-04 into secrets.ps1 of the
               simulated-data folder.
   Version   : v2 (2026-11-01)   Changes: v1 logged in with e-mail + password, which does not
               work with Google sign-in. v2 uses a ThingsBoard API key typed in by the author.
   AI usage  : Written with AI assistance (Claude); run and checked by the author.
   Secrets   : the API key and the tokens are never printed. Never commit secrets.ps1.
   Use       : .\thingsboard_setup_devices_20261101_v2.ps1          (dry run: shows the plan)
               .\thingsboard_setup_devices_20261101_v2.ps1 -Apply   (asks for the API key and does it)
   ============================================================================= #>
param(
  [switch]$Apply,
  [string]$TbHost = "thingsboard.cloud",
  [string]$SecretsFolder = (Join-Path $PSScriptRoot "..\simulated_events_to_thingsboard_20261101_v1")
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$label = "simulated data"
$points = [ordered]@{
  "MP-02" = [ordered]@{ measuring_point_id="MP-02"; location="Electrical installation"; category="Cable offcuts";
             ewc_code="17 04 11"; point_type="scale"; tare_weight=2000; change_threshold=100;
             removal_meaning="unknown"; valid_from="2026-09-01" }
  "MP-03" = [ordered]@{ measuring_point_id="MP-03"; location="Machining"; category="Metal chips";
             ewc_code="12 01 01"; point_type="scale"; tare_weight=5000; change_threshold=1000;
             removal_meaning="unknown"; valid_from="2026-09-01" }
 
}

# ---- dry run ------------------------------------------------------------------------
Write-Host ""
Write-Host "Plan on $TbHost :"
foreach ($n in $points.Keys) {
  Write-Host ("  {0}: create if missing, label '{1}', server attributes:" -f $n, $label)
  foreach ($k in $points[$n].Keys) { Write-Host ("      {0} = {1}" -f $k, $points[$n][$k]) }
}
Write-Host ("  MP-01: label -> '{0}' (attributes not changed)" -f $label)
Write-Host ("  Tokens of MP-01..MP-04 -> {0}\secrets.ps1 (not printed)" -f $SecretsFolder)
if (-not $Apply) { Write-Host ""; Write-Host "Dry run: nothing was changed. Add -Apply to do it."; exit }

# ---- API key (Account -> Security -> API keys, typed by the author, kept only in memory) ---
$sec = Read-Host "Paste the ThingsBoard API key (it is not shown)" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
$key = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
if (-not $key) { Write-Host "No API key given. Stopped."; exit 1 }
$base = "https://$TbHost/api"
$h = @{ "X-Authorization" = "ApiKey $key" }
$key = $null
try {
  $me = Invoke-RestMethod -Uri "$base/auth/user" -Headers $h
  Write-Host ("API key accepted (user: {0})" -f $me.email)
} catch {
  Write-Host "The API key was not accepted (wrong, disabled or expired). Stopped."; exit 1
}

function Get-Device([string]$name) {
  try { return Invoke-RestMethod -Uri ("$base/tenant/devices?deviceName=" + [uri]::EscapeDataString($name)) -Headers $h }
  catch {
    $code = $null
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    if ($code -eq 404 -or $_.Exception.Message -match '404|not found') { return $null } else { throw }
  }
}
function Save-Device($dev) {
  $json = $dev | ConvertTo-Json -Depth 10
  return Invoke-RestMethod -Uri "$base/device" -Method Post -Headers $h -ContentType "application/json" `
           -Body ([Text.Encoding]::UTF8.GetBytes($json))
}

$ids = @{}
# ---- MP-02..MP-04: create, label, attributes ------------------------------------------
foreach ($n in $points.Keys) {
  $dev = Get-Device $n
  if (-not $dev) {
    $dev = Save-Device ([ordered]@{ name = $n; type = "default"; label = $label })
    Write-Host ("{0}: created" -f $n)
  } elseif ($dev.label -ne $label) {
    $dev.label = $label; $dev = Save-Device $dev
    Write-Host ("{0}: existed, label set" -f $n)
  } else { Write-Host ("{0}: existed" -f $n) }
  $ids[$n] = $dev.id.id
  $attrJson = $points[$n] | ConvertTo-Json
  Invoke-RestMethod -Uri ("$base/plugins/telemetry/DEVICE/{0}/attributes/SERVER_SCOPE" -f $dev.id.id) -Method Post `
    -Headers $h -ContentType "application/json" -Body ([Text.Encoding]::UTF8.GetBytes($attrJson)) | Out-Null
  Write-Host ("{0}: {1} server attributes written" -f $n, $points[$n].Count)
}

# ---- MP-01: only the label ---------------------------------------------------------------
$mp01 = Get-Device "MP-01"
if (-not $mp01) { Write-Host "MP-01 not found. Stopped."; exit 1 }
if ($mp01.label -ne $label) { $mp01.label = $label; $mp01 = Save-Device $mp01; Write-Host "MP-01: label set" }
$ids["MP-01"] = $mp01.id.id

# ---- tokens -> secrets.ps1 of the simulated-data folder ------------------------------------
if (-not (Test-Path $SecretsFolder)) { New-Item -ItemType Directory -Path $SecretsFolder | Out-Null }
$lines = @("# secrets.ps1 - written by thingsboard_setup_devices_20261101_v2.ps1. Never send, show or commit.",
           ('$TB_HOST   = "{0}"' -f $TbHost), '$TB_TOKENS = @{')
foreach ($n in @("MP-01","MP-02","MP-03")) {
  $c = Invoke-RestMethod -Uri ("$base/device/{0}/credentials" -f $ids[$n]) -Headers $h
  if ($c.credentialsType -ne "ACCESS_TOKEN") { Write-Host ("{0}: credentials are not an access token. Stopped." -f $n); exit 1 }
  $lines += ('  "{0}" = "{1}"' -f $n, $c.credentialsId)
}
$lines += '}'
Set-Content -Path (Join-Path $SecretsFolder "secrets.ps1") -Value $lines -Encoding ASCII
Write-Host ("Tokens of MP-01..MP-04 written to {0}\secrets.ps1 (not shown)." -f $SecretsFolder)
Write-Host "Done."
