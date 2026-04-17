# Air Quality Monitor

[中文说明](README.md)

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-E7352C)
![Target](https://img.shields.io/badge/Target-ESP32--S3-00979D)
![Home%20Assistant](https://img.shields.io/badge/Home%20Assistant-MQTT%20Discovery-41BDF5)
![OTA](https://img.shields.io/badge/OTA-GitHub%20Release%20%2B%20Local-2EA44F)

`Air Quality Monitor` is an indoor air-quality monitoring node based on `YD-ESP32-S3`. It is designed for long-running local-network use, collecting `CO2`, temperature, humidity, pressure, `VOC / NOx`, particulate matter, and particle-count data, then turning those readings into easier-to-use air-quality signals.

The project is not just a sensor reader. It aims to be a practical local air-quality terminal:

- First-time provisioning through `BLE`, then local management through a web console
- Automatic `Home Assistant` integration through `MQTT Discovery`
- Realtime air-quality readings, trends, diagnostics, and a small set of useful control entities
- Local OTA upload and direct upgrade from `GitHub Releases`
- Degraded operation when one sensor is offline, while remaining sensors continue publishing

## Capabilities

Core data collected and presented by the device:

- Environment readings: `CO2`, temperature, humidity, pressure, `VOC Index`, `NOx Index`
- Particle readings: `PM1.0 / PM2.5 / PM4.0 / PM10`, particle-count concentration, typical particle size
- Derived signals: estimated `PM AQI`, composite air quality, particle profile, pressure trend, humidity trend, dew-point spread, and short-term rain outlook
- Device state: network status, sensor availability, sample age, firmware version, and recent errors

The firmware prefers live `BMP390` pressure data for dynamic `SCD41` compensation. If `BMP390` is unavailable, it falls back to the configured altitude compensation. `SGP41` follows Sensirion conditioning and learning behavior, and `VOC / NOx` are only treated as valid after stabilization.

## Hardware

Default hardware setup:

- Controller: `YD-ESP32-S3`
- `SCD41`: `CO2 / temperature / humidity`
- `SGP41`: `VOC Index / NOx Index`
- `BMP390`: pressure / temperature, also used for `SCD41` pressure compensation
- `SPS30`: particulate mass concentration, particle-count concentration, and typical particle size
- On-board `WS2812 RGB`: follows the composite air-quality state

The current wiring uses one shared `I2C` bus for `SCD41 / SGP41 / BMP390`, while `SPS30` stays on a separate `UART`.

Default pins:

| Function | Pin |
| --- | --- |
| Shared `I2C SDA` | `GPIO8` |
| Shared `I2C SCL` | `GPIO9` |
| `SPS30 UART TX/RX` | `GPIO17 / GPIO18` |
| On-board `RGB` | `GPIO48` |

See [docs/wiring-top-view.svg](docs/wiring-top-view.svg) for the wiring diagram. All sensors must share ground. `SCD41 / SGP41 / BMP390` use `3.3V I2C`; `SPS30` uses `5V` power and keeps `SEL` floating to enable `UART`.

## Quick Start

Requirements:

- `ESP-IDF v6.0`
- A flashable `ESP32-S3` development board
- Connected sensor modules

Build and flash:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

To change pins, publish interval, GitHub OTA repository settings, or other compile-time options:

```bash
idf.py menuconfig
```

## First Use

When the device has no saved `Wi-Fi` credentials, it enters `BLE` provisioning mode.

1. Use Espressif's `ESP BLE Prov` app or a compatible client.
2. Connect to the `BLE Service Name` `airmon-<device_id>`.
3. Enter the `PoP`, which defaults to `<device_id>`.
4. Send `Wi-Fi` credentials and let the device join the local network.
5. Open the assigned IP address in a browser to access the local web console.

`device_id` is derived from the last 3 bytes of the device `MAC`, for example `a1b2c3`. If the device stays offline for a long time, it falls back to `BLE` provisioning so network settings can be updated.

The public repository does not include default `Wi-Fi` credentials. With only `Wi-Fi` configured, the device can be managed on the LAN, but `MQTT` will not start until an `MQTT URL` is saved.

## Local Web Console

The web console is embedded into the firmware. Its source lives in `components/provisioning_web/web/`.

The console is used to:

- View realtime air quality, sensor readings, trends, and device diagnostics
- Configure `MQTT URL`
- Configure `SCD41` altitude compensation, temperature offset, `ASC`, and `FRC`
- Control `SPS30` sampling, fan cleaning, and the status LED
- Trigger `Home Assistant Discovery` republish
- Run local OTA, GitHub OTA, restart, or factory reset

The console works on desktop and mobile browsers and supports Simplified Chinese / English switching. It currently has no authentication, so it should only be used on trusted local networks.

## Home Assistant

The device integrates with `Home Assistant` through `MQTT Discovery`. Configure a single `MQTT URL` in the web console, for example:

```text
mqtt://user:password@192.168.1.20:1883
```

Supported format:

```text
mqtt://[user:password@]host[:port]
```

After integration, the device publishes:

- Main environment entities: `CO2 / temperature / humidity / pressure / VOC / NOx / PM / particle counts`
- Derived entities: `PM AQI`, composite air quality, particle profile, trends, and short-term rain outlook
- Useful diagnostics: `Wi-Fi RSSI`, uptime, sample age, IP address, firmware version, recent error, compensation source, and `SGP41` stabilization countdown
- Control entities: `SCD41 ASC`, `SPS30 Sleep`, `SPS30 Fan Cleaning`, `RGB Status LED`, `Restart`, `Factory Reset`, `Republish Discovery`, and `Apply SCD41 FRC`

The firmware avoids exposing large amounts of low-value internal state to `Home Assistant`. When `Discovery` is republished, deprecated retained discovery entities from older firmware versions are also cleared.

## OTA

The firmware supports two upgrade paths:

- Local OTA: upload an OTA application image from the web console
- GitHub OTA: check and download a new version from project `GitHub Releases`

Release assets normally use `air_quality_monitor-ota-<version>.bin` for web OTA. OTA updates the whole application, including the embedded web console.

If you fork this repository and want to keep GitHub OTA, update the release owner, repository, and asset prefix in the compile-time configuration.

## Project Structure

| Path | Purpose |
| --- | --- |
| `main/` | Application entry point, provisioning, main loop, and status LED |
| `components/sensors/` | Sensor drivers, sampling, compensation, and persisted sensor state |
| `components/air_quality/` | `AQI`, composite air quality, particle profile, and rain heuristic |
| `components/device_state_json/` | Shared state JSON builder for Web and MQTT |
| `components/mqtt_ha/` | `MQTT` publishing, `Home Assistant Discovery`, and remote controls |
| `components/provisioning_web/` | Local web console, HTTP API, and frontend assets |
| `components/ota_manager/` | Local OTA and GitHub OTA |
| `components/platform/` | Persistent config, `Wi-Fi`, and platform defaults |
| `.github/workflows/` | Release build and publishing workflows |
| `tools/` / `scripts/` | Local checks, versioning, and release helpers |

## Limitations

- A missing sensor does not block the whole device, but its corresponding data becomes empty or invalid.
- `SGP41 VOC / NOx` need learning and stabilization time, so readings are not immediately used as valid air-quality inputs after boot.
- The rain outlook is a heuristic based on indoor pressure, humidity, and seasonal context. It is not a weather forecast.
- The web console currently has no authentication and should be deployed only on trusted local networks.
- If the browser still shows the old UI after OTA, it is usually a cache issue. Force-refresh once.
