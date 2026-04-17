# Air Quality Monitor

[English](README.en.md)

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-E7352C)
![Target](https://img.shields.io/badge/Target-ESP32--S3-00979D)
![Home%20Assistant](https://img.shields.io/badge/Home%20Assistant-MQTT%20Discovery-41BDF5)
![OTA](https://img.shields.io/badge/OTA-GitHub%20Release%20%2B%20Local-2EA44F)

`Air Quality Monitor` 是一个基于 `YD-ESP32-S3` 的室内空气质量监测节点。它面向局域网长期运行，采集 `CO2`、温湿度、气压、`VOC / NOx`、颗粒物和粒子数，并把这些数据整理成更容易理解的空气质量结论。

项目重点不是只把传感器数值读出来，而是把设备做成一个可以日常使用的本地空气质量终端：

- 通过 `BLE` 完成首次配网，之后在局域网内通过 Web 后台管理
- 自动通过 `MQTT Discovery` 接入 `Home Assistant`
- 提供实时空气质量、趋势、诊断和少量必要控制实体
- 支持本地 OTA 上传，也支持从 `GitHub Releases` 在线升级
- 单个传感器离线时不阻断整机，其余数据继续发布

## 能力概览

设备采集和展示的核心数据包括：

- 环境数据：`CO2`、温度、湿度、气压、`VOC Index`、`NOx Index`
- 颗粒物数据：`PM1.0 / PM2.5 / PM4.0 / PM10`、粒子数浓度、典型粒径
- 派生判断：`PM AQI` 估算、综合空气质量、粒径画像、气压趋势、湿度趋势、露点差、短时降雨可能
- 设备状态：网络、传感器可用性、采样年龄、固件版本、最近错误

固件会优先使用 `BMP390` 的实时气压为 `SCD41` 做动态补偿；如果 `BMP390` 不可用，会回退到配置里的海拔补偿。`SGP41` 按 Sensirion 的 conditioning 和学习流程运行，`VOC / NOx` 会在稳定后才作为有效数据使用。

## 硬件组成

当前默认硬件方案：

- 主控：`YD-ESP32-S3`
- `SCD41`：`CO2 / 温度 / 湿度`
- `SGP41`：`VOC Index / NOx Index`
- `BMP390`：气压 / 温度，并用于 `SCD41` 气压补偿
- `SPS30`：颗粒物质量浓度、粒子数浓度和典型粒径
- 板载 `WS2812 RGB`：跟随综合空气质量显示状态

当前接线方案以一条共享 `I2C` 总线连接 `SCD41 / SGP41 / BMP390`，`SPS30` 单独使用 `UART`。

默认引脚：

| 功能 | 引脚 |
| --- | --- |
| 共享 `I2C SDA` | `GPIO8` |
| 共享 `I2C SCL` | `GPIO9` |
| `SPS30 UART TX/RX` | `GPIO17 / GPIO18` |
| 板载 `RGB` | `GPIO48` |

接线示意图见 [docs/wiring-top-view.svg](docs/wiring-top-view.svg)。所有传感器需要共地；`SCD41 / SGP41 / BMP390` 使用 `3.3V I2C`，`SPS30` 使用 `5V` 供电并保持 `SEL` 悬空以启用 `UART`。

## 快速开始

开发环境：

- `ESP-IDF v6.0`
- 可刷写的 `ESP32-S3` 开发板
- 已连接的传感器模块

构建和烧录：

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

如果需要修改引脚、发布周期、GitHub OTA 仓库等编译配置，可以运行：

```bash
idf.py menuconfig
```

## 首次使用

设备没有保存过 `Wi-Fi` 时，会进入 `BLE` 配网模式。

1. 使用 Espressif 官方 `ESP BLE Prov` App 或兼容客户端连接设备。
2. 选择 `airmon-<device_id>` 这个 `BLE Service Name`。
3. 输入 `PoP`，默认是 `<device_id>`。
4. 下发 `Wi-Fi` 后，设备会加入局域网。
5. 设备拿到 IP 后，用浏览器打开该 IP 进入本地 Web 后台。

`device_id` 来自设备 `MAC` 后 3 字节，例如 `a1b2c3`。设备长时间离线时会自动回退到 `BLE` 配网模式，方便重新配置网络。

公开仓库不会内置默认 `Wi-Fi` 凭据。只配置 `Wi-Fi` 时设备可以进入局域网，但不会启动 `MQTT`；保存 `MQTT URL` 后才会连接 broker 并发布 `Home Assistant Discovery`。

## 本地 Web 后台

Web 后台会随固件一起嵌入设备，源码位于 `components/provisioning_web/web/`。

后台主要用于：

- 查看实时空气质量、传感器数据、趋势和设备诊断
- 配置 `MQTT URL`
- 配置 `SCD41` 海拔补偿、温度偏移、`ASC` 和 `FRC`
- 控制 `SPS30` 采样、风扇清洁和状态灯
- 触发 `Home Assistant Discovery` 重发
- 执行本地 OTA、GitHub OTA、重启或恢复出厂

后台支持桌面和手机浏览器，也支持简体中文 / English 切换。当前没有登录认证，建议只在可信局域网内使用。

## Home Assistant

设备通过 `MQTT Discovery` 自动接入 `Home Assistant`。在 Web 后台填写一行 `MQTT URL` 即可，例如：

```text
mqtt://user:password@192.168.1.20:1883
```

支持的格式是：

```text
mqtt://[user:password@]host[:port]
```

接入后默认发布：

- 主要环境实体：`CO2 / 温度 / 湿度 / 气压 / VOC / NOx / PM / 粒子数`
- 派生实体：`PM AQI`、综合空气质量、粒径画像、趋势和短时降雨判断
- 有价值的诊断实体：`Wi-Fi RSSI`、运行时间、采样年龄、IP、固件版本、最近错误、补偿来源、`SGP41` 稳定剩余时间
- 控制实体：`SCD41 ASC`、`SPS30 Sleep`、`SPS30 Fan Cleaning`、`RGB Status LED`、`Restart`、`Factory Reset`、`Republish Discovery`、`Apply SCD41 FRC`

固件会避免向 `Home Assistant` 暴露大量低价值内部状态。重新发布 `Discovery` 时，也会清理旧版中已经废弃的 retained discovery 实体。

## OTA

固件支持两种升级方式：

- 本地 OTA：在 Web 后台上传 OTA 应用镜像
- GitHub OTA：从项目的 `GitHub Releases` 检查并下载新版本

发布产物中通常使用 `air_quality_monitor-ota-<version>.bin` 做 Web OTA。OTA 会升级整个应用，包括嵌入式 Web 后台。

如果 fork 本仓库并希望继续使用 GitHub OTA，需要在编译配置中把 release owner、repo 和 asset 前缀改成自己的仓库信息。

## 项目结构

| 路径 | 作用 |
| --- | --- |
| `main/` | 应用入口、配网、主循环和状态灯 |
| `components/sensors/` | 传感器驱动、采样、补偿和持久化状态 |
| `components/air_quality/` | `AQI`、综合空气质量、粒径画像、降雨启发式等计算 |
| `components/device_state_json/` | Web 和 MQTT 共用的状态 JSON 构建 |
| `components/mqtt_ha/` | `MQTT` 发布、`Home Assistant Discovery` 和远程控制 |
| `components/provisioning_web/` | 本地 Web 后台、HTTP API 和前端资源 |
| `components/ota_manager/` | 本地 OTA 和 GitHub OTA |
| `components/platform/` | 配置持久化、`Wi-Fi` 和平台默认值 |
| `.github/workflows/` | Release 构建和发布流程 |
| `tools/` / `scripts/` | 本地检查、版本号和发布辅助脚本 |

## 运行限制

- 缺少单个传感器不会阻断整机运行，但对应数据会显示为空或无效。
- `SGP41 VOC / NOx` 需要学习和稳定时间，刚上电时读数不会立即作为有效空气质量依据。
- 降雨判断是室内气压、湿度和季节上下文的启发式结果，不是天气预报。
- 当前 Web 后台没有认证，部署时应放在可信局域网内。
- OTA 后如果浏览器仍显示旧页面，通常是缓存问题，强制刷新一次即可。
