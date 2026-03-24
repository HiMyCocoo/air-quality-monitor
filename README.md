# air-quality-monitor

基于 `YD-ESP32-S3 + Sensirion SCD41 + SGP41 + SPS30` 的室内空气质量监测节点。

当前固件设计很明确：

- 启动时优先尝试已保存的 `Wi-Fi`；没有已保存配置时，当前默认构建还会先尝试编译进固件的默认 `Wi-Fi`
- 没有可用网络或联网失败超时后，自动回退到 `ESP BLE Prov`
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
- 总体空气质量评估：优先使用 `PM AQI` 估算（基于 `EPA PM AQI` 分段），并补充 `CO2 / 湿度` 室内提示
- 直观补充评级：为 `CO2` 提供通风状态标签，为 `VOC / NOx Index` 提供 `Sensirion` 相对事件等级，方便快速判断当前状态
- 颗粒物画像：综合 `PM1.0 / PM2.5 / PM4.0 / PM10 / 粒子数 / 典型粒径` 给出粒径分布描述
- 本地网页：状态、遥测、`Wi-Fi / MQTT URL` 配置、传感器控制、`RGB` 灯控制、Discovery 重发、OTA、重启、恢复出厂
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

### 1. 默认联网尝试

如果设备里还没有已保存的配置，当前仓库默认构建出来的固件会先尝试连接一组编译时默认 `Wi-Fi`：

- `SSID: <your-wifi-ssid>`
- `Password: <your-wifi-password>`

这组默认值主要用于开发和联调。如果你不希望带着这组凭据发布固件，应在构建前改掉它。

### 2. BLE 配网回退

没有可用 `Wi-Fi`、默认 `Wi-Fi` 连接失败、或离线超时后，设备会进入 `ESP BLE Prov`。

- BLE Service Name：`airmon-<device_id>`
- PoP：`<device_id>`

这里的 `device_id` 是设备 `MAC` 后 3 字节的小写十六进制字符串，例如 `a1b2c3`。

### 3. 接入局域网

用 Espressif 官方 `ESP BLE Prov` App 或兼容客户端下发 `Wi-Fi` 后，设备会自动连入局域网。

### 4. 打开本地网页补 MQTT

设备拿到 IP 后，用浏览器访问设备 IP，在网页里填写一行 `MQTT URL`：

- 例如：`mqtt://user:password@192.168.1.20:1883`

保存后设备会自动重启。

说明：

- 只配 `Wi-Fi` 也能进入局域网
- 只有 `MQTT URL` 配好后，设备才会启动 MQTT 并发布 Discovery
- `MQTT URL` 格式为 `mqtt://[user:password@]host[:port]`
- 用户名或密码里如果有 `@ / :` 等保留字符，需要做 `URL 编码`
- `Discovery Prefix / Topic Root / Publish Interval` 不再由网页输入，而是固定使用固件默认值
- 恢复出厂后如果现场默认 `Wi-Fi` 仍然可连，设备会直接回到该网络，不一定停留在 `BLE` 配网页面

## 本地网页控制台

网页控制台支持：

- 查看设备状态、传感器在线状态、最近错误
- 查看 `Composite Air Quality / PM AQI 估算 / CO2 Ventilation Status / VOC Event Level / NOx Event Level / Particle Profile / 温湿度 / PM / 粒子数 / 典型粒径 / 样本年龄`
- 修改 `Wi-Fi / MQTT URL` 配置
- 设置 `SCD41` 海拔补偿和温度偏移
- 控制 `SCD41 ASC`
- 应用 `SCD41 FRC`
- 控制 `SPS30` 休眠 / 唤醒
- 控制板载 `RGB` 状态灯开关
- 重新发布 `Home Assistant Discovery`
- OTA 升级
- 重启设备、恢复出厂

当前网页管理端口不带登录认证，只适合你信任的局域网。

网页前端文件当前在 `components/provisioning_web/index.html`，构建时会作为嵌入资源打进固件，而不是独立存储在文件系统里。

### OTA 说明

- OTA 升级的是整包应用镜像，不只是后端逻辑
- 因为管理后台前端已经嵌入固件，所以 OTA 会连前端页面一起升级
- 如果 OTA 后浏览器里样式还是旧的，通常是缓存问题，强制刷新一次即可

## MQTT / Home Assistant

默认值：

- `device_name = aq-monitor-<device_id>`
- `discovery_prefix = homeassistant`
- `topic_root = air_quality_monitor/<device_id>`
- `mqtt_port = 1883`
- `publish_interval_sec = 10`

网页后台里，`MQTT` 现在只需要填写一行 `MQTT URL`，例如：

- `mqtt://user:password@192.168.1.20:1883`

`Discovery 前缀 / 主题根路径 / 发布间隔` 不再由用户输入，而是固定使用上面的代码默认值。

自动发布的主要实体：

- `CO2 / 温度 / 相对湿度`
- `VOC Index / NOx Index`
- `CO2 Ventilation Status / VOC Event Level / NOx Event Level`
- `Particle Profile / Particle Profile Note / Sample Age`
- `PM1.0 / PM2.5 / PM4.0 / PM10.0`
- `粒子数浓度`
- `典型粒径`
- `Composite Air Quality / Basis / Driver / Note`
- `PM AQI Estimate / AQI 等级 / 主要污染物`
- `Wi-Fi RSSI / Uptime / Heap / Firmware Version / Last Error / IP / AP SSID / Device ID`
- `Provisioning Mode / Wi-Fi Connected / MQTT Connected / Sensors Ready`
- `SCD41 / SGP41 / SPS30 / RGB 状态灯` 在线状态
- `SCD41 / SGP41 / PM` 数据有效状态，以及 `SGP41 Conditioning`
- `SCD41 ASC`
- `SPS30 Sleep`
- `RGB Status LED`
- `Restart / Factory Reset / Republish Discovery / Apply SCD41 FRC`

## 运行说明

- 缺少某一颗传感器时，系统仍会启动，其他在线传感器照常工作
- `SGP41` 上电后会先经历一小段 `NOx` 调理期，随后进入算法学习阶段
- 总体评估优先使用 `PM AQI` 估算（基于 `EPA PM AQI` 分段），`CO2 / 湿度` 只作为美国室内指导补充提示
- `CO2 Ventilation Status` 是通风状态分级；`VOC / NOx Event Level` 基于 Sensirion 指数的相对事件强度，不代表绝对浓度
- `Particle Profile` 现在优先看 `PM2.5 / PM10` 的细颗粒占比，再用 `SPS30` 的分段质量峰值、数量峰值和典型粒径做校正，更接近常见气溶胶里的细模态 / 粗模态判断
- 其中 `PM2.5 / PM10 <= 0.35` 被视作明显偏粗颗粒信号，`>= 0.60` 视作明显偏细颗粒信号；这参考了公开文献里的粉尘事件经验值和 `EPA` 的细 / 粗颗粒分界，但仍然是工程解释层，不是官方分级标准
- `VOC / NOx Index`、`PM1.0 / PM4.0`、粒子数和典型粒径会继续上报，但不参与 `EPA AQI`
- 板载 `RGB` 现在直接跟随统一的总体评估结果
- 传感器都还没准备好时，板载灯会进入等待态闪烁
