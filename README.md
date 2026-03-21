# ESP32-S3 Air Monitor

ESP32-S3 firmware for:

- `SCD41` over I2C for `CO2 / temperature / humidity`
- `PMS7003` over UART for `PM1.0 / PM2.5 / PM10 / particle counts`
- `MQTT Discovery` integration with Home Assistant
- `SoftAP + local web console + OTA upload`

## Wiring

### ESP32-S3 default pin mapping

These are the firmware defaults currently compiled into `Kconfig`:

- `SCD41 SDA -> GPIO8`
- `SCD41 SCL -> GPIO9`
- `PMS7003 RXD -> GPIO17` on sensor side, so wire `ESP32 GPIO17 (TX)` to `PMS7003 PIN7 RX`
- `PMS7003 TXD -> GPIO18` on sensor side, so wire `ESP32 GPIO18 (RX)` to `PMS7003 PIN9 TX`
- `PMS7003 SET -> GPIO16`
- `PMS7003 RESET -> GPIO15`

### Power

- `SCD41` uses `3.3V`
- `PMS7003` power pins `PIN1/PIN2` must use `5V`
- `PMS7003` UART, `SET`, and `RESET` are `3.3V TTL`
- All grounds must be common

### PMS7003 pin summary

- `PIN1 VCC -> 5V`
- `PIN2 VCC -> 5V`
- `PIN3 GND -> GND`
- `PIN4 GND -> GND`
- `PIN5 RESET -> GPIO15`
- `PIN7 RX -> GPIO17`
- `PIN9 TX -> GPIO18`
- `PIN10 SET -> GPIO16`

Notes from the sensor documentation reflected in the firmware:

- `SET` high or floating means normal work mode
- `SET` low means sleep mode
- `RESET` is active low
- After waking the PMS7003 from sleep, the firmware ignores samples for `30s` to allow fan stabilization

## Features

- Stores Wi-Fi and MQTT settings in NVS
- Boots into `SoftAP` if configuration is missing
- Falls back to `SoftAP` again after prolonged Wi-Fi outage
- Publishes Home Assistant discovery, state, diagnostics, and command topics
- Exposes buttons/switches/number entities for restart, factory reset, republish discovery, ASC, PMS sleep, and SCD41 forced recalibration
- Hosts an unauthenticated local web console for configuration, diagnostics, and OTA upload

## Home Assistant topics

The device publishes under:

- `air_monitor/<device_id>/state`
- `air_monitor/<device_id>/diag`
- `air_monitor/<device_id>/availability`
- `air_monitor/<device_id>/cmd/...`

Discovery prefix defaults to `homeassistant`.

## Build

This workspace directory currently has a trailing space in its filesystem name. `idf.py` path validation does not handle that cleanly, so the reliable build path is:

```bash
ln -s "/Users/yifan/Documents/Vscode/particulate_matter_CO2_sensor " /tmp/airmon-src
. /Users/yifan/.espressif/v5.5.3/esp-idf/export.sh
cd /tmp/airmon-src
idf.py -B /tmp/airmon-build build
```

The current successful build output was generated in:

- `/tmp/airmon-build/air_monitor.bin`

## Flash

```bash
. /Users/yifan/.espressif/v5.5.3/esp-idf/export.sh
cd /tmp/airmon-src
idf.py -B /tmp/airmon-build -p /dev/cu.usbmodem* flash monitor
```

Replace the serial port with the actual device path on your machine.

## Web Console

After boot:

- If no config exists, connect to the device AP and open `http://192.168.4.1/`
- If the device is already on Wi-Fi, open the STA IP shown in the serial log or your router DHCP list

The current project intentionally leaves the local admin page unauthenticated, matching the requested trusted-LAN deployment model.
