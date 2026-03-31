# Air Quality Monitor

[English](README.en.md)

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-E7352C)
![Target](https://img.shields.io/badge/Target-ESP32--S3-00979D)
![Home%20Assistant](https://img.shields.io/badge/Home%20Assistant-MQTT%20Discovery-41BDF5)
![OTA](https://img.shields.io/badge/OTA-GitHub%20Release%20%2B%20Local-2EA44F)

基于 `YD-ESP32-S3` 的室内空气质量监测节点，面向局域网部署和 `Home Assistant` 集成。固件集成 `BLE` 配网、本地响应式 Web 管理后台、`MQTT Discovery`、手动 OTA，以及基于 `GitHub Releases` 的在线升级。

## 项目特性

- 当前硬件方案使用一条共享 `I2C` 总线接 `SCD41 / SGP41 / BMP390`，`SPS30` 保持 `UART`
- 同时输出原始传感器数据、`PM AQI` 估算、综合空气质量结论、粒径画像、气压趋势和短时降雨可能
- `BMP390` 在运行时为 `SCD41` 提供动态气压补偿，`BMP390` 不可用时自动回退到 `SCD41` 海拔补偿
- `SGP41` 按 Sensirion 官方 conditioning 和学习流程运行，`VOC / NOx` 只有在稳定后才标记为有效
- 缺少单个传感器时不会阻断整机启动，其余在线传感器继续工作
- 本地 Web 后台同时适配桌面和手机访问
- 自动接入 `Home Assistant`，同时暴露状态类实体和控制类实体
- 支持本地固件上传 OTA，也支持直接从 `GitHub Releases` 检查和下载升级

## 传感器与导出能力

| 模块 | 接口 | 主要输出 | 备注 |
| --- | --- | --- | --- |
| `SCD41` | `I2C` | `CO2 / 温度 / 相对湿度` | 支持海拔补偿、温度偏移、`ASC`、`FRC` |
| `SGP41` | `I2C` | `VOC Index / NOx Index` | 带 conditioning、学习剩余时间和有效性状态 |
| `BMP390` | `I2C` | `气压 / 温度` | 给 `SCD41` 提供动态气压补偿 |
| `SPS30` | `UART` | `PM1.0 / PM2.5 / PM4.0 / PM10.0 / 粒子数浓度 / 典型粒径` | 支持连续采样、休眠、唤醒、风扇清洁 |
| 固件衍生指标 | 软件计算 | `PM AQI` 估算、综合空气质量、粒径画像、湿度趋势、气压趋势、露点差、短时降雨可能 | 降雨启发式带杭州季节上下文 |

> 说明：项目当前以本 README 中的共享 `I2C` 布局为准，旧版“双 I2C 总线”接法不再是当前方案。

## 默认硬件配置

| 项目 | 默认值 |
| --- | --- |
| 开发板 | `YD-ESP32-S3` |
| 共享 `I2C` 总线 | `GPIO8 / GPIO9` |
| `SPS30` 串口 | `UART1` on `GPIO17 / GPIO18` |
| 板载状态灯 | `WS2812 RGB` on `GPIO48` |
| `BMP390` 默认地址 | `0x77` |
| 默认发布周期 | `10` 秒 |
| `SPS30` 自动风扇清洁周期 | `604800` 秒（7 天） |
| 运行离线回退到 BLE 配网超时 | `180` 秒 |

当前默认拓扑：

- `SCD41 / SGP41 / BMP390` 接到同一个 `I2C` 分线器
- 分线器上游只占用开发板一组 `SDA / SCL`
- `SPS30` 独立走 `UART`
- 板载 `RGB` 为开发板自带，无需额外接线

## 默认接线

共享 `I2C` 总线：

- 开发板 `GPIO8 -> SDA`
- 开发板 `GPIO9 -> SCL`
- 开发板 `3V3 -> I2C 分线器 VCC`
- 开发板 `GND -> I2C 分线器 GND`
- `SCD41 / SGP41 / BMP390` 都按标准 `I2C` 四线连接到分线器

`SPS30`：

- `VDD -> 5V0`
- `RX -> GPIO17`，即开发板发出、接传感器 `RX`
- `TX -> GPIO18`，即开发板接收、接传感器 `TX`
- `SEL -> 悬空，不接`
- `GND -> GND`

接线注意：

- `SCD41 / SGP41 / BMP390` 都是 `3.3V I2C`
- `SPS30` 继续走 `UART`，不要把 `SEL` 拉到 `GND`
- 所有模块必须共地

## 快速开始

前提：

- `ESP-IDF v6.0`
- 可正常供电和刷写的 `ESP32-S3` 开发板
- 至少一颗已接好的传感器

构建和烧录示例：

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

常用开发命令：

```bash
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

建议优先使用开发板的 `USB to UART` 口烧录和查看日志。

## 首次上电与配网

1. 如果设备里还没有已保存的 `Wi-Fi` 配置，固件会直接进入 `BLE` 配网流程。
2. 默认 `BLE Service Name` 为 `airmon-<device_id>`。
3. 默认 `PoP` 为 `<device_id>`。
4. `device_id` 来自设备 `MAC` 后 3 字节，例如 `a1b2c3`。
5. 用 Espressif 官方 `ESP BLE Prov` App 或兼容客户端下发 `Wi-Fi` 后，设备自动加入局域网。
6. 设备拿到 IP 后，用浏览器打开设备 IP，即可访问本地管理后台。
7. 如果设备长时间离线，固件会自动回退到 `BLE` 配网模式。

说明：

- 公开代码库不内置任何默认 `Wi-Fi` 凭据
- 只配置 `Wi-Fi` 时设备可以进局域网，但不会启动 `MQTT`
- 只有 `MQTT URL` 保存成功后，设备才会启动 `MQTT` 并发布 `Home Assistant Discovery`
- 更新 `MQTT URL` 会触发设备重启；只修改 `SCD41` 补偿参数时会即时下发到传感器

## 本地 Web 管理后台

后台源码位于 `components/provisioning_web/index.html`，构建时会嵌入固件。

后台主要包含三个区域：

- 实时监测：综合空气质量、`PM AQI` 估算、`CO2 / 温湿度 / VOC / NOx / PM / 粒子数 / 粒径画像 / 气压趋势 / 短时降雨可能`
- 项目配置：`MQTT URL`、`SCD41` 海拔补偿、温度偏移、`ASC`
- 维护操作：`SPS30` 连续采样开关、风扇清洁、`RGB` 状态灯、重发 Discovery、`FRC`、手动 OTA、GitHub OTA、重启、恢复出厂

当前后台特性：

- 支持桌面和手机访问
- 保存 `MQTT URL` 时不会回显已保存的用户名和密码
- 支持从页面查看 `SGP41` 学习状态、`BMP390` 补偿来源和设备运行诊断
- 当前无登录认证，只适合信任的局域网

## MQTT / Home Assistant

默认配置：

| 项目 | 默认值 |
| --- | --- |
| `device_name` | `aq-monitor-<device_id>` |
| `discovery_prefix` | `homeassistant` |
| `topic_root` | `air_quality_monitor/<device_id>` |
| `mqtt_port` | `1883` |
| `publish_interval_sec` | `10` |

页面里只需要填写一行 `MQTT URL`，例如：

```text
mqtt://user:password@192.168.1.20:1883
```

`MQTT URL` 规则：

- 格式为 `mqtt://[user:password@]host[:port]`
- 只支持主机、端口、用户名和密码
- 用户名或密码里如果有 `@ / :` 等保留字符，需要先做 URL 编码

默认会向 `Home Assistant` 暴露以下几类实体：

- 环境数据：`CO2 / 温度 / 湿度 / VOC / NOx / PM / 粒子数 / 典型粒径`
- 派生数据：`PM AQI` 估算、综合空气质量、粒径画像、气压趋势、湿度趋势、露点差、短时降雨可能
- 诊断数据：`Wi-Fi RSSI / Uptime / Heap / IP / Device ID / Firmware Version / Last Error`
- 状态实体：`Provisioning Mode / Wi-Fi Connected / MQTT Connected / All Sensors Ready`
- 控制实体：`SCD41 ASC / SPS30 Sleep / SPS30 Fan Cleaning / RGB Status LED / Restart / Factory Reset / Republish Discovery / Apply SCD41 FRC`

## OTA 与 GitHub Release

固件支持两种升级方式：

- 本地手动上传 OTA
- 直接从 `GitHub Releases` 检查并升级

本地 OTA 说明：

- 页面上传的是 OTA 应用镜像，不是整包串口刷机镜像
- 推荐上传发布产物中的 `air_quality_monitor-ota-<version>.bin`
- `OTA` 会升级整个应用，包括嵌入式 Web 前端

GitHub OTA 默认编译配置：

| 配置项 | 默认值 |
| --- | --- |
| `AIRMON_GITHUB_RELEASE_OWNER` | `HiMyCocoo` |
| `AIRMON_GITHUB_RELEASE_REPO` | `air-quality-monitor` |
| `AIRMON_GITHUB_OTA_ASSET_PREFIX` | `air_quality_monitor-ota-` |

如果你 fork 这个仓库并想保留在线升级能力，记得同步修改这些配置。

仓库已包含自动发布工作流：

- 推送到 `main` 或 `master` 时，工作流会按 `PROJECT_VER` 的 `主版本.次版本` 基线自动递增 `patch`
- 工作流会构建固件、创建或复用 `vX.Y.Z` tag，并发布到 `GitHub Releases`
- 也可以手动运行 `Build And Release Firmware`，为已有 tag 补发资产

当前 Release 产物：

- `air_quality_monitor-ota-<version>.bin`
- `air_quality_monitor-full-<version>.bin`
- `bootloader-<version>.bin`
- `partition-table-<version>.bin`
- `ota_data_initial-<version>.bin`
- `flasher_args-<version>.json`
- `SHA256SUMS.txt`

## 项目结构

| 路径 | 作用 |
| --- | --- |
| `main/` | 应用入口、状态灯逻辑、配网与主循环 |
| `components/sensors/` | `SCD41 / SGP41 / BMP390 / SPS30` 驱动与采样 |
| `components/air_quality/` | `AQI`、综合空气质量、粒径画像、降雨启发式等计算 |
| `components/mqtt_ha/` | `MQTT` 发布、`Home Assistant Discovery`、远程控制实体 |
| `components/provisioning_web/` | 本地 Web 管理后台和 HTTP API |
| `components/platform/` | 配置持久化、`Wi-Fi` 启动与平台配置 |
| `.github/workflows/` | GitHub Release 构建与发布流程 |
| `scripts/` | 版本号和发布辅助脚本 |

## 运行说明与限制

- 缺少单个传感器不会阻断整机运行，但对应字段会显示为空或无效
- `SCD41` 补偿优先级为：配置中的海拔补偿与温度偏移先应用，`BMP390` 有效时切到动态气压补偿，`BMP390` 中断后再回退
- `SGP41 VOC` 在当前配置下约需 `1.5` 小时稳定，`NOx` 约需 `5.8` 小时稳定
- 板载 `RGB` 状态灯跟随综合空气质量结论；如果传感器还未准备好，会进入等待态闪烁
- `OTA` 后如果浏览器仍显示旧样式，通常是缓存问题，强制刷新一次即可
- 降雨启发式当前使用 `CST-8` 和杭州季节上下文，如果你部署在其他地区，建议按实际场景调整
