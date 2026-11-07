# LHKK by-product tracking system

Final code and platform configuration of the bachelor thesis *Developing a data-driven by-product tracking system to support consistent data collection and sharing* (Duxing Li, HAMK, 2026, for Lounais-Hämeen koulutuskuntayhtymä LHKK). Only the final versions are kept here; earlier versions are in the git history.

## Folders

| Folder | Content | Thesis |
|---|---|---|
| `firmware/measuring_point_firmware_20261031_v6/` | Firmware of one measuring point (ESP32 + HX711): state machine, events, HTTPS upload to ThingsBoard | 4.6.2–4.6.3, 4.7 |
| `tools/hx711_raw_reader_20261018_v3/` | Raw HX711 reader used for the calibration of the bench platform | 4.6.2 |
| `tools/smartkitchen_to_thingsboard_20261101_v2/` | PowerShell script which reads the SmartKitchen API and writes daily totals to the device SK-01 (used for the history) | 4.6.4 |
| `tools/thingsboard_setup_devices_20261101_v2/` | PowerShell script which created the devices MP-02 and MP-03 and their configuration attributes (run it with `-SecretsFolder ..\simulated_events_to_thingsboard_20261103_v2`) | 4.6.4 |
| `tools/simulated_events_to_thingsboard_20261103_v2/` | **Simulated** data: generator, uploader and the two CSV files (7.9.–2.10.2026 and 3.10.–2.11.2026) | 4.7 |
| `thingsboard/rule_chain/` | Rule chain "SmartKitchen daily import": exported JSON and its two scripts (the unconnected node "Load token" is left from an earlier plan and is not used) | 4.6.4 |
| `thingsboard/calculated_fields/` | Calculated fields "By-product flows" and "Next emptying forecast" | 4.6.4, 4.7 |
| `thingsboard/dashboard/` | Exported dashboard "LHKK By-Product Dashboard" (overview, field detail and kitchen detail pages) | 4.7 |
| `figures/` | Sources of figures (SVG) | 4.6 |

## Simulated data

MP-01, MP-02 and MP-03 contain **simulated** example data based on the author's estimates, not measurements. The generator uses a fixed random seed, so it produces the same file every time. `simulated_events_20261101_v1.csv` (7.9.–2.10.2026) was made by the earlier generator v1; it also has rows for MP-04, which were not uploaded because the ThingsBoard Free plan allows only five devices. In real use each container (for example each lathe's chip bin) has its own measuring point.

## Naming rule

`<purpose>_<YYYYMMDD>_v<n>`. The date is the day the version was first written. A change in behaviour is a new version.

## Secrets

Wi-Fi passwords, ThingsBoard device tokens and the SmartKitchen token are **not** in this repository (see `.gitignore`). Each folder that needs them has a `secrets.example.*` file: copy it to `secrets.h` / `secrets.ps1` and fill it in on your own computer. In the rule chain the SmartKitchen token is read from ThingsBoard Secrets storage (`${secret:smartkitchen_token;type:TEXT}`).

## AI use

Parts of the code were written with AI assistance (Claude) and reviewed, run and tested by the author, as stated in the thesis.
