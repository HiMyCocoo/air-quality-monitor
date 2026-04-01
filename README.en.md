# Air Quality Monitor

[中文说明](README.md)

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-E7352C)
![Target](https://img.shields.io/badge/Target-ESP32--S3-00979D)
![Home%20Assistant](https://img.shields.io/badge/Home%20Assistant-MQTT%20Discovery-41BDF5)
![OTA](https://img.shields.io/badge/OTA-GitHub%20Release%20%2B%20Local-2EA44F)

An indoor air-quality monitoring node built around `YD-ESP32-S3` for local-network deployment and `Home Assistant` integration. The firmware includes `BLE` provisioning, a responsive local web console with Simplified Chinese / English switching, `MQTT Discovery`, manual OTA, and direct upgrades from `GitHub Releases`.

## Highlights

- Current hardware layout uses one shared `I2C` bus for `SCD41 / SGP41 / BMP390`, while `SPS30` stays on `UART`
- Exposes both raw sensor data and derived signals such as `PM AQI`, composite air-quality status, particle profile, pressure trend, and short-term rain outlook
- `BMP390` provides runtime ambient-pressure compensation for `SCD41`, with automatic fallback to configured `SCD41` altitude compensation
- `SGP41` follows Sensirion conditioning and learning behavior, and `VOC / NOx` are only marked valid after stabilization
- The firmware keeps working when one sensor is missing; other online sensors continue reporting
- The local web console is usable on both desktop and mobile browsers, with Simplified Chinese / English switching
- Integrates with `Home Assistant` through `MQTT Discovery`, including both telemetry and control entities
- Supports both local file OTA and direct upgrade from `GitHub Releases`

## Sensors and Derived Outputs

| Module | Interface | Main Outputs | Notes |
| --- | --- | --- | --- |
| `SCD41` | `I2C` | `CO2 / temperature / relative humidity` | Supports altitude compensation, temperature offset, `ASC`, and `FRC` |
| `SGP41` | `I2C` | `VOC Index / NOx Index` | Includes conditioning, stabilization countdown, and validity state |
| `BMP390` | `I2C` | `pressure / temperature` | Supplies dynamic pressure compensation to `SCD41` |
| `SPS30` | `UART` | `PM1.0 / PM2.5 / PM4.0 / PM10.0 / particle counts / typical particle size` | Supports continuous sampling, sleep, wake, and fan cleaning |
| Derived firmware signals | Software | `PM AQI`, composite air quality, particle profile, humidity trend, pressure trend, dew-point spread, short-term rain outlook | Rain heuristic uses Hangzhou seasonal context |

> Note: the current project uses the shared `I2C` layout described in this README. The older dual-`I2C` wiring is no longer the active hardware design.

## Default Hardware Layout

| Item | Default |
| --- | --- |
| Development board | `YD-ESP32-S3` |
| Shared `I2C` bus | `GPIO8 / GPIO9` |
| `SPS30` serial interface | `UART1` on `GPIO17 / GPIO18` |
| On-board status LED | `WS2812 RGB` on `GPIO48` |
| `BMP390` default address | `0x77` |
| Default publish interval | `10` seconds |
| `SPS30` auto fan-cleaning interval | `604800` seconds (7 days) |
| Runtime offline timeout before BLE fallback | `180` seconds |

Default topology:

- `SCD41 / SGP41 / BMP390` share the same `I2C` splitter
- The splitter uses only one `SDA / SCL` pair on the board
- `SPS30` stays on a separate `UART`
- The `RGB` status LED is already built into the board

## Default Wiring

Shared `I2C` bus:

- Board `GPIO8 -> SDA`
- Board `GPIO9 -> SCL`
- Board `3V3 -> I2C splitter VCC`
- Board `GND -> I2C splitter GND`
- Connect `SCD41 / SGP41 / BMP390` to the splitter using normal 4-wire `I2C`

`SPS30`:

- `VDD -> 5V0`
- `RX -> GPIO17`, meaning board TX to sensor `RX`
- `TX -> GPIO18`, meaning board RX to sensor `TX`
- `SEL -> leave floating`
- `GND -> GND`

Wiring notes:

- `SCD41 / SGP41 / BMP390` are `3.3V I2C` devices
- `SPS30` must remain on `UART`; do not pull `SEL` to `GND`
- All modules must share the same ground

## Quick Start

Requirements:

- `ESP-IDF v6.0`
- An `ESP32-S3` board that can be powered and flashed reliably
- At least one correctly wired sensor

Build and flash example:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

Common development commands:

```bash
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

Using the board's `USB to UART` port is recommended for flashing and logs.

## First Boot and Provisioning

1. If no `Wi-Fi` credentials are stored, the firmware enters `BLE` provisioning automatically.
2. The default `BLE Service Name` is `airmon-<device_id>`.
3. The default `PoP` is `<device_id>`.
4. `device_id` is derived from the last 3 bytes of the device `MAC`, for example `a1b2c3`.
5. Use Espressif's `ESP BLE Prov` app or another compatible client to send `Wi-Fi` credentials.
6. Once the device gets an IP address, open that IP in a browser to reach the local web console.
7. If the device stays offline long enough, it falls back to `BLE` provisioning automatically.

Notes:

- The public codebase does not ship with default `Wi-Fi` credentials
- `Wi-Fi` alone is enough for LAN access, but `MQTT` does not start until `MQTT URL` is configured
- `Home Assistant Discovery` is published only after `MQTT URL` is saved successfully
- Updating `MQTT URL` triggers a device restart, while `SCD41` compensation-only changes are applied immediately

## Local Web Console

The frontend source is `components/provisioning_web/index.html`, and it is embedded into the firmware at build time.

The console is organized into three main areas:

- Realtime monitoring: composite air quality, `PM AQI`, `CO2 / temperature / humidity / VOC / NOx / PM / particle counts / particle profile / pressure trend / short-term rain outlook`
- Project configuration: `MQTT URL`, `SCD41` altitude compensation, temperature offset, and `ASC`
- Maintenance: `SPS30` continuous sampling, fan cleaning, `RGB` status LED, Discovery republish, `FRC`, manual OTA, GitHub OTA, restart, and factory reset

Current console characteristics:

- Works on both desktop and mobile browsers
- Supports live switching between Simplified Chinese and English, with the choice persisted in the browser
- Saved `MQTT` credentials are not echoed back into the page
- Shows `SGP41` learning state, `BMP390` compensation source, and device diagnostics
- No authentication is implemented; use it only on trusted local networks

## MQTT / Home Assistant

Defaults:

| Item | Default |
| --- | --- |
| `device_name` | `aq-monitor-<device_id>` |
| `discovery_prefix` | `homeassistant` |
| `topic_root` | `air_quality_monitor/<device_id>` |
| `mqtt_port` | `1883` |
| `publish_interval_sec` | `10` |

The web console accepts a single `MQTT URL`, for example:

```text
mqtt://user:password@192.168.1.20:1883
```

`MQTT URL` rules:

- Format: `mqtt://[user:password@]host[:port]`
- Only host, port, username, and password are supported
- Reserved characters such as `@ / :` in username or password must be URL-encoded

By default, the firmware exposes these entity categories to `Home Assistant`:

- Environment telemetry: `CO2 / temperature / humidity / VOC / NOx / PM / particle counts / typical particle size`
- Derived signals: `PM AQI`, composite air quality, particle profile, pressure trend, humidity trend, dew-point spread, and short-term rain outlook
- Diagnostics: `Wi-Fi RSSI / Uptime / Heap / IP / Device ID / Firmware Version / Last Error`
- State entities: `Provisioning Mode / Wi-Fi Connected / MQTT Connected / All Sensors Ready`
- Control entities: `SCD41 ASC / SPS30 Sleep / SPS30 Fan Cleaning / RGB Status LED / Restart / Factory Reset / Republish Discovery / Apply SCD41 FRC`

## OTA and GitHub Releases

The firmware supports two upgrade paths:

- Local manual OTA upload
- Direct upgrade from `GitHub Releases`

Manual OTA notes:

- The upload page expects the OTA application image, not the full serial-flash image
- Use `air_quality_monitor-ota-<version>.bin` from the release assets when possible
- `OTA` upgrades the whole application, including the embedded web frontend

Default compile-time GitHub OTA settings:

| Config | Default |
| --- | --- |
| `AIRMON_GITHUB_RELEASE_OWNER` | `HiMyCocoo` |
| `AIRMON_GITHUB_RELEASE_REPO` | `air-quality-monitor` |
| `AIRMON_GITHUB_OTA_ASSET_PREFIX` | `air_quality_monitor-ota-` |

If you fork this repository and want to keep direct GitHub OTA, update these values accordingly.

The repository already contains an automated release workflow:

- Pushes to `main` or `master` use `PROJECT_VER` as the `major.minor` baseline and auto-increment the `patch`
- The workflow builds firmware, creates or reuses a matching `vX.Y.Z` tag, and publishes to `GitHub Releases`
- You can also run `Build And Release Firmware` manually to republish assets for an existing tag

Current release assets:

- `air_quality_monitor-ota-<version>.bin`
- `air_quality_monitor-full-<version>.bin`
- `bootloader-<version>.bin`
- `partition-table-<version>.bin`
- `ota_data_initial-<version>.bin`
- `flasher_args-<version>.json`
- `SHA256SUMS.txt`

## Project Structure

| Path | Purpose |
| --- | --- |
| `main/` | Application entry point, status LED logic, provisioning, and main loop |
| `components/sensors/` | Drivers and sampling logic for `SCD41 / SGP41 / BMP390 / SPS30` |
| `components/air_quality/` | `AQI`, composite air-quality logic, particle profile, and rain heuristic |
| `components/mqtt_ha/` | `MQTT` publishing, `Home Assistant Discovery`, and remote control entities |
| `components/provisioning_web/` | Local web console and HTTP API |
| `components/platform/` | Persistent config, `Wi-Fi`, and platform-level settings |
| `.github/workflows/` | GitHub release build and publishing workflow |
| `scripts/` | Versioning and release helper scripts |

## Runtime Notes and Limitations

- Missing one sensor does not block the rest of the system; corresponding fields simply become empty or invalid
- `SCD41` compensation priority is: configured altitude and temperature offset first, then dynamic `BMP390` pressure compensation when available, then fallback to altitude compensation if `BMP390` stops updating
- With the current defaults, `SGP41 VOC` usually needs about `1.5` hours to stabilize, and `NOx` about `5.8` hours
- The on-board `RGB` LED follows the composite air-quality result; when sensors are not ready yet, it enters a waiting blink pattern
- If the browser still shows the old UI after `OTA`, it is usually a cache issue; force-refresh once
- The rain heuristic currently assumes `CST-8` and Hangzhou seasonal context; adjust it if you deploy in a different region
