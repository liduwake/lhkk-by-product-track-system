# LHKK by-product tracking system

Code and figure sources of the bachelor thesis *Developing a data-driven by-product tracking system to support consistent data collection and sharing* (Duxing Li, HAMK, for LHKK).

## Folders

| Folder | Content |
|---|---|
| `tools/` | Bench tools: raw HX711 readers used for the measurements in the thesis |
| `firmware/` | Firmware of one measuring point (ESP32): state machine and ThingsBoard uplink |
| `figures/` | Sources of figures (SVG) |

## Naming rule

`<purpose>_<YYYYMMDD>_v<n>`. The date is the day the version was first written. An old version is never changed. A change in behaviour (also in the output format) is a new version. Every version has a git tag with the same name.

## Secrets

`firmware/.../secrets.h` holds the Wi-Fi name, the Wi-Fi password and the ThingsBoard device token. It is **not** in the repository (see `.gitignore`). Copy `secrets.example.h` to `secrets.h` and fill it in on your own computer.

## Records

Which versions were tested, on what, and where AI was used is recorded in the thesis project (code log and AI usage log). The commit message of each version says what changed.
