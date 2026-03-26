# air-quality-monitor

基于 `YD-ESP32-S3` 的室内空气质量监测节点。

当前项目按这套硬件方案维护：

- `SPS30` 保留 `UART`
- `SCD41`、`SGP41`、`BMP390` 通过 `I2C` 分线器共用一条 `I2C` 总线接入 `ESP32-S3`
- `BMP390` 提供温度和气压，当前用于本地管理后台、诊断状态和 `Home Assistant` 遥测展示
- 设备连网后提供本地管理后台，并通过 `MQTT Discovery` 接入 `Home Assistant`

旧版“双 I2C 总线”接法不再作为当前项目方案。

## 当前项目内容

- `SCD41`：`CO2 / 温度 / 相对湿度`
- `SGP41`：`VOC Index / NOx Index`
- `BMP390`：`气压 / 温度`
- `SPS30`：`PM1.0 / PM2.5 / PM4.0 / PM10.0`
- `SPS30`：`0.5 / 1.0 / 2.5 / 4.0 / 10 μm` 粒子数浓度
- `SPS30`：`典型粒径`
- 总体空气质量评估：优先使用 `PM AQI` 估算，并补充 `CO2 / 湿度 / VOC / NOx` 的解释性状态
- 管理后台和 `Home Assistant` 同时展示 `BMP390` 温度、气压、传感器状态
- 允许缺少部分传感器继续运行；单个传感器异常时，其余在线传感器继续上报
- 板载 `WS2812 RGB`（`GPIO48`）用于实时空气质量状态指示

## 硬件与默认 GPIO

- 开发板：`YD-ESP32-S3`
- 共享 `I2C` 总线：`GPIO8 / GPIO9`
- `SPS30`：`UART1`，`GPIO17 / GPIO18`
- 板载 `WS2812 RGB`：`GPIO48`
- `BMP390` 默认地址：`0x77`

默认拓扑：

- `SCD41 / SGP41 / BMP390` 都接到同一个 `I2C` 分线器
- 分线器上游只占用开发板一组 `SDA / SCL`
- `SPS30` 单独走 `UART`

如果你要改引脚，入口仍然在 `idf.py menuconfig` 对应的项目配置项里调整。

## 默认接线

### 共享 I2C 总线

- 开发板 `GPIO8 -> I2C SDA`
- 开发板 `GPIO9 -> I2C SCL`
- 开发板 `3V3 -> I2C 分线器 VCC`
- 开发板 `GND -> I2C 分线器 GND`

把以下三个模块都接到分线器的任一支路上：

- `SCD41`
- `SGP41`
- `BMP390`

三者都按标准 `I2C` 四线连接：

- `SDA -> SDA`
- `SCL -> SCL`
- `VDD/VCC -> 3V3`
- `GND -> GND`

### SPS30

- 原厂 5Pin：`VDD / RX / TX / SEL / GND`
- `VDD -> 5V0`
- `RX -> GPIO17`（开发板发出，接传感器 `RX`）
- `TX -> GPIO18`（开发板接收，接传感器 `TX`）
- `SEL -> 悬空，不接`
- `GND -> GND`

接线注意：

- `SCD41 / SGP41 / BMP390` 都是 `3.3V I2C`
- `SPS30` 继续走 `UART`，不要把 `SEL` 拉到 `GND`
- 所有 `GND` 必须共地
- 板载 `RGB` 是开发板自带的，不需要外接

## SCD41 补偿说明

当前固件对 `SCD41` 已接入的补偿项有两项：

1. `SCD41` 海拔补偿
2. `SCD41` 温度偏移

它们会在启动时加载，并在你通过管理后台保存配置后立即下发到传感器。

当前 `BMP390` 已经完成采集、状态诊断、本地页面展示和 `MQTT / Home Assistant` 上报，但还没有接入 `SCD41` 的运行时 `CO2` 气压补偿链路。

## 烧录

先加载你的 `ESP-IDF` 环境。当前机器上的示例路径是：

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

如果你的 `ESP-IDF` 安装路径不同，把 `export.sh` 换成你自己的路径即可。

建议优先使用板子右侧 `USB to UART` 口烧录和看日志。

## 自动发布到 GitHub Release

仓库现在可以按 `PROJECT_VER` 自动发布：

- `CMakeLists.txt` 里的 `PROJECT_VER` 现在作为版本基线使用
- workflow 会读取它的 `主版本.次版本`，并自动递增 `patch`
- 例如基线是 `0.1.0`，已有最新 tag 是 `v0.1.3`，下一次发布会自动变成 `v0.1.4`
- 如果你把基线改成 `0.2.0`，后续发布就会切到 `0.2.x`
- 然后在同一个 workflow 里继续构建固件，并自动发布到 GitHub Release
- Release 会附带自动生成的更新日志和编译后的 `bin` 文件

当前上传到 Release 的产物包括：

- OTA 应用固件 `air_quality_monitor-ota-<version>.bin`
- 完整刷机镜像 `air_quality_monitor-full-<version>.bin`
- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin`
- `flasher_args.json`
- `SHA256SUMS.txt`

对应配置文件：

- 自动打 tag、构建并发布：`.github/workflows/release-on-tag.yml`
- Release notes 分类配置：`.github/release.yml`

使用方式：

1. 平时正常提交并推送到 `main` 或 `master`
2. workflow 会自动计算下一个 patch 版本、创建 tag、构建并发布
3. 只有当你想切换大版本或小版本时，才需要手动修改 `CMakeLists.txt` 里的 `PROJECT_VER`

如果某个提交已经有对应的 `v*` tag，workflow 会复用这个 tag，不会在同一个提交上继续递增出新的 patch 版本。

如果某个 tag 已经存在，但之前因为工作流失败没有成功上传资产，也可以手动运行 `Build And Release Firmware`，并在可选输入里填上对应 tag（例如 `v0.1.0`）来补发 Release 资产。

## 首次上电与配网

### 1. 默认联网尝试

如果设备里还没有已保存的配置，当前仓库默认不会内置 `Wi-Fi` 凭据。

也就是说，首次上电通常会进入 `BLE` 配网流程；只有你自己在私有构建里额外加了默认凭据，设备才会先尝试自动联网。
### 2. BLE 配网回退

没有可用 `Wi-Fi`、默认 `Wi-Fi` 连接失败、或离线超时后，设备会进入 `ESP BLE Prov`。

- BLE Service Name：`airmon-<device_id>`
- PoP：`<device_id>`

其中 `device_id` 是设备 `MAC` 后 3 字节的小写十六进制字符串，例如 `a1b2c3`。

### 3. 接入局域网

用 Espressif 官方 `ESP BLE Prov` App 或兼容客户端下发 `Wi-Fi` 后，设备会自动连入局域网。

### 4. 打开本地后台补 MQTT

设备拿到 IP 后，用浏览器访问设备 IP，在管理后台里填写一行 `MQTT URL`：

- 例如：`mqtt://user:password@192.168.1.20:1883`

保存后设备会自动重启。

说明：

- 只配 `Wi-Fi` 也能进入局域网
- 只有 `MQTT URL` 配好后，设备才会启动 `MQTT` 并发布 Discovery
- `MQTT URL` 格式为 `mqtt://[user:password@]host[:port]`
- 用户名或密码里如果有 `@ / :` 等保留字符，需要做 `URL 编码`

## 本地管理后台

管理后台提供以下内容：

- 设备状态、联网状态、最近错误
- `CO2 / 温度 / 相对湿度 / VOC / NOx / PM / 粒子数 / 典型粒径`
- `BMP390` 温度、当前气压、`BMP390 Ready`、`BMP390 Valid`
- `Wi-Fi / MQTT URL` 配置
- `SCD41` 温度偏移
- `SCD41` 海拔补偿
- `SCD41 ASC`
- `SCD41 FRC`
- `SPS30` 休眠 / 唤醒
- `SPS30` 手动触发自动风扇清洁
- 板载 `RGB` 状态灯开关
- `Home Assistant Discovery` 重发
- `OTA` 升级
- 重启、恢复出厂

配置语义说明：

- `SCD41 温度偏移`：始终生效
- `SCD41 海拔补偿`：当前直接作为 `SCD41` 的补偿配置项使用

当前网页管理端口不带登录认证，只适合你信任的局域网。

网页前端文件在 `components/provisioning_web/index.html`，构建时会嵌入固件。

## OTA 说明

- `OTA` 升级的是整包应用镜像，不只是后端逻辑
- 因为管理后台前端已经嵌入固件，所以 `OTA` 会连前端页面一起升级
- 如果 `OTA` 后浏览器里样式还是旧的，通常是缓存问题，强制刷新一次即可

## MQTT / Home Assistant

默认值：

- `device_name = aq-monitor-<device_id>`
- `discovery_prefix = homeassistant`
- `topic_root = air_quality_monitor/<device_id>`
- `mqtt_port = 1883`
- `publish_interval_sec = 10`

网页后台里，`MQTT` 现在只需要填写一行 `MQTT URL`，例如：

- `mqtt://user:password@192.168.1.20:1883`

主要上报内容包括：

- `CO2 / 温度 / 相对湿度`
- `VOC Index / NOx Index`
- `BMP390 温度 / 气压`
- `PM1.0 / PM2.5 / PM4.0 / PM10.0`
- `粒子数浓度`
- `典型粒径`
- `Composite Air Quality / Basis / Driver / Note`
- `PM AQI Estimate / AQI 等级 / 主要污染物`
- `Wi-Fi RSSI / Uptime / Heap / Firmware Version / Last Error / IP / AP SSID / Device ID`
- `Provisioning Mode / Wi-Fi Connected / MQTT Connected / All Sensors Ready`
- `SCD41 / SGP41 / BMP390 / SPS30 / RGB 状态灯` 在线状态
- `SCD41 / SGP41 / BMP390 / PM` 数据有效状态
- `BMP390 Ready / BMP390 Valid`
- `SCD41 ASC`
- `SPS30 Sleep`
- `SPS30 Fan Cleaning`
- `RGB Status LED`
- `Restart / Factory Reset / Republish Discovery / Apply SCD41 FRC`

## 运行说明

- 缺少某一颗传感器时，系统仍会启动，其他在线传感器继续工作
- `BMP390` 不可用不会阻断其他传感器上报；本地后台和 `MQTT` 中的气压 / `BMP390` 温度字段会显示为空，`BMP390 Ready / Valid` 会反映当前状态
- `SGP41` 上电后会先经历一小段 `NOx` 调理期，随后进入算法学习阶段
- `All Sensors Ready` 的语义应理解为四个物理传感器都 ready：`SCD41 / SGP41 / BMP390 / SPS30`
- `CO2 Ventilation Status` 是通风状态分级；`VOC / NOx Event Level` 基于 `Sensirion` 指数的相对事件强度，不代表绝对浓度
- `VOC / NOx Index`、粒子数、典型粒径会继续上报，但不参与 `EPA AQI`
- 板载 `RGB` 跟随统一的总体空气质量评估结果
- 传感器都还没准备好时，板载灯进入等待态闪烁
