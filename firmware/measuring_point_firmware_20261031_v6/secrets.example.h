// secrets.example.h - copy this file to secrets.h; belongs to measuring_point_firmware_20261031_v6
// Fill in the values on your own computer. Never send, show, screenshot or commit secrets.h.
// Wi-Fi: 2.4 GHz, WPA2 password (a phone hotspot works).
// Token: ThingsBoard -> Devices & assets -> Devices -> your device -> Check connectivity (HTTP);
//        the token is the part between /api/v1/ and /telemetry.
// SECRET_MP_ID must be the same as the device name in ThingsBoard (MP-TEST for bench tests, MP-01 later).
#pragma once

#define SECRET_WIFI_SSID      "YOUR_WIFI_NAME"
#define SECRET_WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define SECRET_TB_TOKEN       "YOUR_DEVICE_ACCESS_TOKEN"
#define SECRET_MP_ID          "MP-TEST"
#define SECRET_TB_HOST        "thingsboard.cloud"   // the address in your browser bar, without https://
