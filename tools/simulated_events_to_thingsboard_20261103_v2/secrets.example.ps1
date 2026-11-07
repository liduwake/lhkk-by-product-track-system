# secrets.example.ps1 - copy this file to secrets.ps1 (same folder) and fill it in on your own computer.
# Never send, show, screenshot or commit secrets.ps1.
# One access token per ThingsBoard device (Devices -> device -> Copy access token).
$TB_HOST   = "thingsboard.cloud"
$TB_TOKENS = @{
  "MP-01" = "YOUR_MP01_TOKEN"
  "MP-02" = "YOUR_MP02_TOKEN"
  "MP-03" = "YOUR_MP03_TOKEN"
}
