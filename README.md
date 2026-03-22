# air-quality-monitor

基于 `YD-ESP32-S3 + Sensirion SCD41 + SGP41 + SPS30` 的室内空气质量监测节点。

当前固件设计很明确：

- 首次配网走 `ESP BLE Prov`
- 设备连上局域网后提供本地网页控制台
- 配好 `MQTT` 后通过 `MQTT Discovery` 自动接入 `Home Assistant`
- 允许缺少部分传感器继续运行，缺失项在网页和 MQTT 中显示为不可用
- 板载 `WS2812 RGB`（`GPIO48`）用于显示实时空气质量

## 当前功能

- `SCD41`：`CO2 / 温度 / 相对湿度`
- `SGP41`：`VOC Index / NOx Index`
- `SPS30`：`PM1.0 / PM2.5 / PM4.0 / PM10.0`
- `SPS30`：`0.5 / 1.0 / 2.5 / 4.0 / 10 μm` 粒子数浓度
- `SPS30`：`典型粒径`
- 本地网页：状态、遥测、`Wi-Fi / MQTT` 配置、传感器控制、OTA、重启、恢复出厂
- `Home Assistant`：自动发现传感器、按钮和控制开关

## 硬件与默认 GPIO

- 开发板：`YD-ESP32-S3`
- SDK：`ESP-IDF v5.5.3`
- `SCD41`：`I2C0`，`GPIO8 / GPIO9`
- `SGP41`：`I2C1`，`GPIO11 / GPIO12`
- `SPS30`：`UART1`，`GPIO17 / GPIO18`
- 板载 `WS2812 RGB`：`GPIO48`

如果你要改引脚，入口在 `idf.py menuconfig -> Air Quality Monitor`。

## 默认接线

下图方向按你提供的 `YD-ESP32-S3` 引脚图来画：天线在上，USB 在下，只高亮当前项目实际用到的孔位。

![默认接线图](docs/wiring-top-view.svg)

### SCD41

- 模块针脚自上而下：`SDA / SCL / VDD / GND`
- `SDA -> GPIO8`
- `SCL -> GPIO9`
- `VDD -> 3V3`
- `GND -> GND`

### SGP41

- 常用 4Pin：`SDA / SCL / VDD / GND`
- `SDA -> GPIO11`
- `SCL -> GPIO12`
- `VDD -> 3V3`
- `GND -> GND`

### SPS30

- 原厂 5Pin：`VDD / RX / TX / SEL / GND`
- `VDD -> 5V0`
- `RX -> GPIO17`（这是 `ESP32 TX`，连到传感器 `RX`）
- `TX -> GPIO18`（这是 `ESP32 RX`，连到传感器 `TX`）
- `SEL -> 悬空，不接`
- `GND -> GND`

接线注意：

- `SCD41` 和 `SGP41` 都是 `3.3V I2C`
- `SPS30` 现在走 `UART`，不要再按旧版 `I2C` 接法把 `SEL` 拉到 `GND`
- 开发板上所有 `GND` 孔位内部互通，哪个位置顺手就接哪个
- 板载 `GPIO48` 的 `RGB` 是开发板自带的，不需要外接

## 烧录

```bash
source ~/.espressif/v5.5.3/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

建议优先使用板子右侧 `USB to UART` 口烧录和看日志。

## 首次上电与配网

### 1. BLE 配网

没有保存过 `Wi-Fi` 凭据时，设备会进入 `ESP BLE Prov`。

- BLE Service Name：`airmon-<device_id>`
- PoP：`<device_id>`

这里的 `device_id` 是设备 `MAC` 后 3 字节的小写十六进制字符串，例如 `a1b2c3`。

### 2. 接入局域网

用 Espressif 官方 `ESP BLE Prov` App 或兼容客户端下发 `Wi-Fi` 后，设备会自动连入局域网。

### 3. 打开本地网页补 MQTT

设备拿到 IP 后，用浏览器访问设备 IP，在网页里填写：

- `MQTT Host`
- `MQTT Port`
- `MQTT Username / Password`（如果有）
- `Discovery Prefix`
- `Topic Root`

保存后设备会自动重启。

说明：

- 只配 `Wi-Fi` 也能进入局域网
- 只有 `MQTT Host` 配好后，设备才会启动 MQTT 并发布 Discovery

## 本地网页控制台

网页控制台支持：

- 查看设备状态、传感器在线状态、最近错误
- 查看 `CO2 / 温湿度 / VOC / NOx / PM / 粒子数 / 典型粒径 / US AQI`
- 修改 `Wi-Fi / MQTT` 配置
- 设置 `SCD41` 海拔补偿和温度偏移
- 控制 `SCD41 ASC`
- 应用 `SCD41 FRC`
- 控制 `SPS30` 休眠 / 唤醒
- OTA 升级
- 重启设备、恢复出厂、重新发布 Discovery

当前网页管理端口不带登录认证，只适合你信任的局域网。

## MQTT / Home Assistant

默认值：

- `device_name = aq-monitor-<device_id>`
- `discovery_prefix = homeassistant`
- `topic_root = air_quality_monitor/<device_id>`
- `mqtt_port = 1883`
- `publish_interval_sec = 10`

自动发布的主要实体：

- `CO2 / 温度 / 相对湿度`
- `VOC Index / NOx Index`
- `PM1.0 / PM2.5 / PM4.0 / PM10.0`
- `粒子数浓度`
- `典型粒径`
- `US AQI / AQI 等级 / 主要污染物`
- `Wi-Fi RSSI / Uptime / Heap / Firmware Version / Last Error`
- `SCD41 ASC`
- `SPS30 Sleep`
- `Restart / Factory Reset / Republish Discovery / Apply SCD41 FRC`

## 运行说明

- 缺少某一颗传感器时，系统仍会启动，其他在线传感器照常工作
- `SGP41` 上电后会先经历一小段 `NOx` 调理期，随后进入算法学习阶段
- 板载 `RGB` 的显示优先级是：`PM / US AQI`，然后 `CO2`，最后才回退到 `VOC / NOx`
- 传感器都还没准备好时，板载灯会进入等待态闪烁
