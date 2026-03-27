# air-quality-monitor

[中文说明](README.md)

An indoor air-quality monitoring node based on `YD-ESP32-S3`.

The project is currently maintained around this hardware layout:

- `SPS30` stays on `UART`
- `SCD41`, `SGP41`, and `BMP390` share one `I2C` bus through an `I2C` splitter and connect to the `ESP32-S3`
- `BMP390` provides temperature and pressure, and also supplies runtime ambient-pressure compensation for `SCD41`
- Once connected to the network, the device exposes a local web console and integrates with `Home Assistant` through `MQTT Discovery`
- `SGP41` follows Sensirion's official gas-index conditioning and learning flow; `VOC/NOx` are only marked valid after each index has stabilized

The old "dual I2C bus" wiring is no longer the current project layout.

## Current Scope

- `SCD41`: `CO2 / temperature / relative humidity`
- `SGP41`: `VOC Index / NOx Index`
- `BMP390`: `pressure / temperature`
- `SPS30`: `PM1.0 / PM2.5 / PM4.0 / PM10.0`
- `SPS30`: particle number concentrations for `0.5 / 1.0 / 2.5 / 4.0 / 10 um`
- `SPS30`: typical particle size
- Overall air-quality assessment: primarily based on estimated `PM AQI`, with explanatory states from `CO2 / humidity / VOC / NOx`
- The web console and `Home Assistant` both show `BMP390` temperature, pressure, sensor readiness, and the current `CO2` compensation source
- The system keeps running with missing sensors; if one sensor fails, the remaining online sensors continue to report
- The on-board `WS2812 RGB` on `GPIO48` is used as a real-time air-quality status indicator

## Hardware and Default GPIO

- Development board: `YD-ESP32-S3`
- Shared `I2C` bus: `GPIO8 / GPIO9`
- `SPS30`: `UART1`, `GPIO17 / GPIO18`
- On-board `WS2812 RGB`: `GPIO48`
- `BMP390` default address: `0x77`

Default topology:

- `SCD41 / SGP41 / BMP390` are all connected to the same `I2C` splitter
- The splitter occupies only one `SDA / SCL` pair on the development board
- `SPS30` remains on a separate `UART`

If you want to change pins, use the project configuration items exposed through `idf.py menuconfig`.

Default sensor policy:

- `SCD41` temperature offset defaults to `0.0°C`
- `SCD41 ASC` is disabled by default to avoid accidental self-learning in unknown ventilation conditions
- `SPS30` auto fan-cleaning interval is explicitly set to `604800 seconds` (7 days)
- `SGP41` uses the shared `GPIO8 / GPIO9` `I2C` bus by default

## Default Wiring

### Shared I2C Bus

- Board `GPIO8 -> I2C SDA`
- Board `GPIO9 -> I2C SCL`
- Board `3V3 -> I2C splitter VCC`
- Board `GND -> I2C splitter GND`

Connect the following three modules to any branches on the splitter:

- `SCD41`
- `SGP41`
- `BMP390`

All three use standard 4-wire `I2C` wiring:

- `SDA -> SDA`
- `SCL -> SCL`
- `VDD/VCC -> 3V3`
- `GND -> GND`

### SPS30

- Factory 5-pin header: `VDD / RX / TX / SEL / GND`
- `VDD -> 5V0`
- `RX -> GPIO17` (board TX, connect to sensor `RX`)
- `TX -> GPIO18` (board RX, connect to sensor `TX`)
- `SEL -> leave floating`
- `GND -> GND`

Wiring notes:

- `SCD41 / SGP41 / BMP390` are `3.3V I2C` devices
- `SPS30` must remain on `UART`; do not pull `SEL` to `GND`
- All grounds must be shared
- The RGB LED is already on the board and needs no external wiring

## SCD41 Compensation

The firmware currently applies two configured `SCD41` compensation values:

1. `SCD41` altitude compensation
2. `SCD41` temperature offset

They are loaded on boot and pushed to the sensor immediately after you save settings through the web console.

In addition, if `BMP390` provides a valid pressure reading during runtime, the firmware continuously applies dynamic pressure compensation through `SCD41 set_ambient_pressure`.

Compensation priority:

1. On boot or after a config update, the firmware first applies the configured `SCD41` altitude and temperature-offset values
2. During runtime, as long as `BMP390` keeps producing valid pressure data, `SCD41` switches to dynamic pressure compensation from `BMP390`
3. If `BMP390` pressure updates stop for a while, the system falls back to the configured `SCD41` altitude compensation

## SGP41 Learning Period

`SGP41` does not produce trustworthy `VOC Index / NOx Index` values immediately after startup.

- The first `10` seconds are used for sensor conditioning
- `VOC Index` is only marked valid after the algorithm stabilizes, which is about `1.5` hours with the current setup
- `NOx Index` takes longer to learn, about `5.8` hours with the current setup
- The web console and `Home Assistant` show separate validity states and remaining stabilization time for `VOC` and `NOx`

This means "SGP41 online" after boot does not automatically mean the gas indexes are already usable.

## Flashing

Load your `ESP-IDF` environment first. On the current machine, the example path is:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

If your `ESP-IDF` installation lives elsewhere, replace `export.sh` with your own path.

Using the board's right-side `USB to UART` port is recommended for flashing and logs.

## Automatic GitHub Releases

The repository can now publish releases automatically from `PROJECT_VER`:

- `PROJECT_VER` in `CMakeLists.txt` is used as the release baseline
- Embedded firmware version resolution uses this priority: `AIRMON_RELEASE_VERSION` environment variable > exact git tag (for example `v0.1.4`) > `PROJECT_VER`
- As a result, firmware built by the release workflow, or built locally from an exact `vX.Y.Z` tag, shows the same version in the web console as the released firmware version
- The workflow reads the baseline `major.minor` and automatically increments `patch`
- For example, if the baseline is `0.1.0` and the latest tag is `v0.1.3`, the next release becomes `v0.1.4`
- If you change the baseline to `0.2.0`, subsequent releases move to `0.2.x`
- The same workflow then builds the firmware and publishes it to GitHub Releases
- The release includes auto-generated notes and compiled binary artifacts

Artifacts currently uploaded to a release:

- OTA application image `air_quality_monitor-ota-<version>.bin`
- Full flash image `air_quality_monitor-full-<version>.bin`
- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin`
- `flasher_args.json`
- `SHA256SUMS.txt`

Relevant files:

- Auto-tag, build, and release workflow: `.github/workflows/release-on-tag.yml`
- Release note category config: `.github/release.yml`

Workflow usage:

1. Commit and push to `main` or `master` as usual
2. The workflow calculates the next patch version, creates the tag, builds, and publishes the release
3. You only need to edit `PROJECT_VER` in `CMakeLists.txt` when you want to bump major or minor

If a commit already has a matching `v*` tag, the workflow reuses that tag instead of generating another patch tag for the same commit.

If a tag already exists but assets were not uploaded because a workflow run failed, you can manually run `Build And Release Firmware` and provide the tag explicitly, for example `v0.1.0`, to republish the release assets.

## First Boot and Provisioning

### 1. Default Network Attempt

If the device does not yet have saved configuration, the firmware first tries the built-in default `Wi-Fi` credentials:

- SSID: `<your-wifi-ssid>`
- Password: `<your-wifi-password>`

Only if that default `Wi-Fi` is unavailable or connection fails will the device fall back to `BLE` provisioning.

### 2. BLE Provisioning Fallback

If no working `Wi-Fi` is available, the default `Wi-Fi` connection fails, or the device stays offline long enough, it enters `ESP BLE Prov`.

- BLE Service Name: `airmon-<device_id>`
- PoP: `<device_id>`

Here `device_id` is the lower-case hexadecimal string built from the last 3 bytes of the device `MAC`, for example `a1b2c3`.

### 3. Join the LAN

Use Espressif's official `ESP BLE Prov` app or another compatible client to send `Wi-Fi` credentials. The device then joins the LAN automatically.

### 4. Open the Local Web Console and Add MQTT

Once the device gets an IP, open that IP in a browser and enter a single `MQTT URL` in the web console:

- For example: `mqtt://user:password@192.168.1.20:1883`

The device restarts automatically after saving.

Notes:

- `Wi-Fi` alone is enough for LAN access
- The device starts `MQTT` and publishes Discovery only after `MQTT URL` is configured
- `MQTT URL` format is `mqtt://[user:password@]host[:port]`
- If the username or password contains reserved characters such as `@ / :`, they must be URL-encoded

## Local Web Console

The local web console provides:

- Device status, network status, and the latest error
- `CO2 / temperature / relative humidity / VOC / NOx / PM / particle counts / typical particle size`
- `BMP390` temperature, current pressure, `BMP390 Ready`, and `BMP390 Valid`
- Separate `SGP41 VOC/NOx` learning states and remaining stabilization time
- Current `CO2` compensation source
- `Wi-Fi / MQTT URL` configuration
- `SCD41` temperature offset
- `SCD41` altitude compensation
- `SCD41 ASC`
- `SCD41 FRC`
- `SPS30` sleep / wake
- Manually trigger `SPS30` fan cleaning
- On-board `RGB` status LED switch
- Republish `Home Assistant Discovery`
- `OTA` upgrade
- Restart and factory reset

Configuration semantics:

- `SCD41 temperature offset`: always applies
- `SCD41 altitude compensation`: applied on boot first, and also used as fallback if `BMP390` dynamic pressure compensation is temporarily unavailable
- `SGP41 VOC / NOx`: formal index values are shown only after their learning phases complete; the page shows remaining time during learning

The current web console has no login or authentication and is only suitable for trusted local networks.

The frontend source lives in `components/provisioning_web/index.html` and is embedded into the firmware at build time.

## OTA Notes

- `OTA` upgrades the whole application image, not only backend logic
- Because the web console frontend is embedded in the firmware, `OTA` upgrades the frontend together with the firmware
- If the browser still shows the old style after `OTA`, it is usually a caching issue; force-refresh once

## MQTT / Home Assistant

Defaults:

- `device_name = aq-monitor-<device_id>`
- `discovery_prefix = homeassistant`
- `topic_root = air_quality_monitor/<device_id>`
- `mqtt_port = 1883`
- `publish_interval_sec = 10`

In the web console, `MQTT` now only needs a single `MQTT URL`, for example:

- `mqtt://user:password@192.168.1.20:1883`

Main reported data includes:

- `CO2 / temperature / relative humidity`
- `VOC Index / NOx Index`
- `BMP390 temperature / pressure`
- `PM1.0 / PM2.5 / PM4.0 / PM10.0`
- Particle number concentrations
- Typical particle size
- `Composite Air Quality / Basis / Driver / Note`
- `PM AQI Estimate / AQI level / dominant pollutant`
- `Wi-Fi RSSI / Uptime / Heap / Firmware Version / Last Error / IP / AP SSID / Device ID`
- `Provisioning Mode / Wi-Fi Connected / MQTT Connected / All Sensors Ready`
- Online state for `SCD41 / SGP41 / BMP390 / SPS30 / RGB status LED`
- Data validity state for `SCD41 / SGP41 / BMP390 / PM`
- `BMP390 Ready / BMP390 Valid`
- `CO2 Compensation Source`
- `SCD41 ASC`
- `SPS30 Sleep`
- `SPS30 Fan Cleaning`
- `RGB Status LED`
- `Restart / Factory Reset / Republish Discovery / Apply SCD41 FRC`

## Runtime Notes

- The system still starts if one sensor is missing, and other online sensors continue to work
- When `BMP390` is healthy, `SCD41` uses dynamic pressure compensation; the local web console also shows the current `CO2` compensation source
- If `BMP390` is unavailable, other sensors continue reporting; pressure and `BMP390` temperature are empty in both the local console and `MQTT`, and compensation source falls back to configured `SCD41` altitude compensation
- After power-on, `SGP41` first goes through a short conditioning phase for `NOx`, then enters the algorithm learning stage
- `All Sensors Ready` should be interpreted as all four physical sensors being ready: `SCD41 / SGP41 / BMP390 / SPS30`
- `CO2 Ventilation Status` is a ventilation grade; `VOC / NOx Event Level` is based on Sensirion's relative gas-index event intensity and does not represent absolute concentration
- `VOC / NOx Index`, particle counts, and typical particle size continue to be reported, but are not part of `EPA AQI`
- The on-board `RGB` LED follows the unified overall air-quality assessment result
- If no sensors are ready yet, the on-board LED blinks in a waiting state
