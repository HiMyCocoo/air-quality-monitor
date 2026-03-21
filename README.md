# ESP32-S3 室内空气质量监测节点

这是一个基于 `ESP32-S3 + SCD41 + PMS7003` 的空气质量监测固件工程，目标是做一台可以长期通电运行、自动接入 Home Assistant、并支持本地网页配置与 OTA 升级的自制空气质量设备。

当前工程已经实现并编译通过以下能力：

- 通过 `SCD41` 采集 `CO2 / 温度 / 相对湿度`
- 通过 `PMS7003` 采集 `PM1.0 / PM2.5 / PM10 / 0.3~10um 粒子计数`
- 通过 `MQTT Discovery` 自动把实体注册到 Home Assistant
- 通过 `SoftAP + 本地网页控制台` 进行首次配网、后续改配置、查看状态、下发控制和 OTA 上传
- 通过 `NVS` 保存 Wi-Fi、MQTT 和设备配置
- 在 Wi-Fi 长时间离线时自动回退到 `AP 模式`

这份 README 尽量把你后续真正会遇到的问题都提前写清楚，包括：

- 该买什么、怎么接线
- 哪根线接哪根线
- 哪些地方必须 5V，哪些地方只能 3.3V
- 固件默认用了哪些 GPIO
- 怎么编译、怎么烧录、怎么首次配置
- Home Assistant 最终会出现哪些实体
- 各个 MQTT 主题长什么样
- 网页控制台具体能做什么
- 当前代码有哪些已知限制和注意事项

---

## 1. 项目目标

本项目面向下面这个场景：

- 在室内长期上电运行
- 周期采集 CO2、温湿度、颗粒物浓度和粒子计数
- 自动接入 Home Assistant，不希望手写大量 YAML
- 允许后续通过局域网页面修改 Wi-Fi / MQTT 参数
- 允许通过网页直接上传新固件 OTA

当前设计是“家用/实验室内网设备”的思路，不是面向公网暴露的产品化方案。因此目前网页管理口默认 **不带登录认证**，这一点是按你之前的要求实现的，使用前请确认设备只运行在你信任的局域网里。

---

## 2. 当前已实现的功能

### 2.1 传感器功能

- `SCD41`
  - 周期测量模式
  - 读取 CO2、温度、湿度
  - 支持温度偏移补偿
  - 支持海拔补偿
  - 支持 ASC 自动自校准开关
  - 支持 FRC 强制校准入口

- `PMS7003`
  - UART 连续接收帧
  - 校验 32 字节数据包
  - 读取 PM1.0 / PM2.5 / PM10
  - 读取 0.3 / 0.5 / 1.0 / 2.5 / 5.0 / 10um 粒子计数
  - 支持休眠/唤醒
  - 按文档要求，唤醒后自动忽略前 `30 秒` 数据用于预热稳定
  - 采用最近 `6` 帧的平均值做平滑

### 2.2 网络与配置功能

- 配置保存在 NVS 中
- 无配置时自动进入 `SoftAP`
- Wi-Fi 已配置时优先进入 `STA 模式`
- Wi-Fi 长时间掉线后自动回退为 `SoftAP`
- 本地网页支持：
  - 查看运行状态
  - 查看传感器数据
  - 配置 Wi-Fi
  - 配置 MQTT
  - 配置 SCD41 补偿参数
  - 切换 ASC
  - 控制 PMS7003 休眠/唤醒
  - 输入并执行 SCD41 FRC
  - 手动重新发布 MQTT Discovery
  - 重启
  - 恢复出厂设置
  - OTA 固件上传

### 2.3 Home Assistant 功能

- MQTT Discovery 自动创建设备和实体
- 自动发布设备在线状态 `online/offline`
- 自动发布测量值
- 自动发布诊断信息
- 自动订阅控制命令

---

## 3. 硬件组成

### 3.1 主控

- `YD-ESP32-S3` 开发板
- ESP32-S3 模组
- 工程当前默认目标：`ESP-IDF v5.5.3`

### 3.2 传感器

- `SCD41`
  - I2C 接口
  - 采集 CO2、温度、湿度
  - 供电使用 `3.3V`

- `PMS7003`
  - UART 接口
  - 采集颗粒物质量浓度和粒子计数
  - 供电使用 `5V`
  - 串口和控制脚是 `3.3V TTL`

### 3.3 建议的附加硬件

- 稳定的 USB 供电线
- 供电能力足够的 5V 电源
- 杜邦线或焊接导线
- 如果长期使用，建议固定外壳并保证：
  - PMS7003 进风口/出风口不被遮挡
  - SCD41 有足够空气交换空间

---

## 4. 接线总览

## 4.1 固件默认 GPIO 分配

这些是当前代码里已经写死的默认引脚，定义在 `components/platform/Kconfig`：

- `SCD41 SDA = GPIO8`
- `SCD41 SCL = GPIO9`
- `PMS7003 UART = UART1`
- `PMS7003 RX GPIO = GPIO18`
- `PMS7003 TX GPIO = GPIO17`
- `PMS7003 SET GPIO = GPIO16`
- `PMS7003 RESET GPIO = GPIO15`

注意这里的“RX GPIO / TX GPIO”是 **ESP32 侧 GPIO 角色**：

- `ESP32 GPIO17` 是给 UART 发数据的 `TX`
- `ESP32 GPIO18` 是给 UART 收数据的 `RX`

所以与 PMS7003 接线时仍然遵循交叉原则：

- `ESP32 TX -> PMS7003 RX`
- `ESP32 RX -> PMS7003 TX`

---

## 4.2 SCD41 接线

SCD41 走 I2C，推荐连接如下：

| SCD41 模块引脚 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| `VCC` | `3V3` | 供电 3.3V |
| `GND` | `GND` | 地线共地 |
| `SDA` | `GPIO8` | I2C 数据线 |
| `SCL` | `GPIO9` | I2C 时钟线 |

### SCD41 接线注意事项

- 确认你手上的 SCD41 是带引脚模块，而不是裸芯片
- 如果模块板自带 I2C 上拉电阻，直接接通常即可
- 如果模块不带上拉，I2C 可能需要外部上拉
- SCD41 建议远离会明显发热的位置，否则温湿度读数会偏

---

## 4.3 PMS7003 接线

你给的文档已经明确了 PMS7003 数字接口定义：

- `PIN1 VCC`：5V
- `PIN2 VCC`：5V
- `PIN3 GND`：GND
- `PIN4 GND`：GND
- `PIN5 RESET`：3.3V TTL，低电平复位
- `PIN6 NC`：悬空
- `PIN7 RX`：串口接收，3.3V TTL
- `PIN8 NC`：悬空
- `PIN9 TX`：串口发送，3.3V TTL
- `PIN10 SET`：3.3V TTL，高电平/悬空正常工作，低电平休眠

### PMS7003 推荐接线表

| PMS7003 引脚 | 信号 | ESP32-S3 引脚 | 说明 |
|---|---|---|---|
| `PIN1` | `VCC` | `5V` | 颗粒物模组主供电 |
| `PIN2` | `VCC` | `5V` | 颗粒物模组主供电 |
| `PIN3` | `GND` | `GND` | 地线 |
| `PIN4` | `GND` | `GND` | 地线 |
| `PIN5` | `RESET` | `GPIO15` | 复位控制，低有效 |
| `PIN6` | `NC` | 不接 | 悬空 |
| `PIN7` | `RX` | `GPIO17` | ESP32 TX 输出到 PMS RX |
| `PIN8` | `NC` | 不接 | 悬空 |
| `PIN9` | `TX` | `GPIO18` | PMS TX 输出到 ESP32 RX |
| `PIN10` | `SET` | `GPIO16` | 工作/休眠控制 |

### PMS7003 必须注意的电气事项

这是最容易出问题的地方，请务必注意：

1. `PMS7003 供电必须是 5V`
   - 文档给出的工作电压是 `Typ 5.0V`
   - 风机需要 5V 驱动

2. `PMS7003 串口与控制脚是 3.3V TTL`
   - `RX`
   - `TX`
   - `SET`
   - `RESET`

3. `ESP32 与 PMS7003 必须共地`
   - 如果 GND 不共地，串口通常不稳定或完全不可用

4. `SET 高电平或悬空 = 正常工作`
   - 当前固件默认会主动驱动 `GPIO16`
   - 因此建议你把 `SET` 线真的接上，不要只留悬空

5. `RESET 低电平有效`
   - 当前固件默认会主动驱动 `GPIO15`
   - 同样建议接上，便于后续恢复

6. `PIN6 / PIN8 保持悬空`

### PMS7003 休眠唤醒说明

根据你提供的文档：

- 传感器进入休眠后风扇会停转
- 醒来后至少需要 `30 秒` 重新稳定

当前代码已经按这个规则处理：

- 设备唤醒 PMS7003 后，不会立刻把读数当作有效值
- 固件会忽略前 `30 秒` 的 PMS 数据，再开始重新给出 PM 值

---

## 4.4 一张完整接线表

如果你按当前默认固件直接接，推荐如下：

| 设备 | 引脚 | 连接到 | 备注 |
|---|---|---|---|
| ESP32-S3 | `3V3` | `SCD41 VCC` | SCD41 供电 |
| ESP32-S3 | `GND` | `SCD41 GND` | 共地 |
| ESP32-S3 | `GPIO8` | `SCD41 SDA` | I2C 数据 |
| ESP32-S3 | `GPIO9` | `SCD41 SCL` | I2C 时钟 |
| ESP32-S3 | `5V` | `PMS7003 PIN1` | PMS 5V 供电 |
| ESP32-S3 | `5V` | `PMS7003 PIN2` | PMS 5V 供电 |
| ESP32-S3 | `GND` | `PMS7003 PIN3` | 共地 |
| ESP32-S3 | `GND` | `PMS7003 PIN4` | 共地 |
| ESP32-S3 | `GPIO15` | `PMS7003 PIN5 RESET` | 低有效复位 |
| ESP32-S3 | `GPIO17` | `PMS7003 PIN7 RX` | UART TX -> PMS RX |
| ESP32-S3 | `GPIO18` | `PMS7003 PIN9 TX` | UART RX <- PMS TX |
| ESP32-S3 | `GPIO16` | `PMS7003 PIN10 SET` | 工作/休眠控制 |

---

## 5. 当前软件架构

工程按职责分成几个部分：

### 5.1 `main/`

负责总控流程：

- 初始化 NVS
- 生成 `device_id`
- 读取保存的配置
- 启动 Wi-Fi
- 决定走 `AP` 还是 `STA`
- 启动网页服务
- 启动传感器任务
- 启动 MQTT
- 周期发布数据
- 处理重启 / 恢复出厂 / AP 回退

入口文件：

- `main/app_main.c`

### 5.2 `components/platform/`

负责平台基础能力：

- 配置默认值
- NVS 读写
- Wi-Fi AP/STA 切换
- 获取 RSSI / IP

### 5.3 `components/sensors/`

负责传感器驱动与采样管理：

- `scd41.c`
- `pms7003.c`
- `sensors.c`

### 5.4 `components/mqtt_ha/`

负责：

- MQTT 建连
- 发布 Discovery
- 发布状态 JSON
- 发布诊断 JSON
- 订阅命令主题
- 处理控制回调

### 5.5 `components/provisioning_web/`

负责：

- 启动本地 HTTP 服务器
- 提供网页控制台
- 返回状态 JSON
- 保存配置
- OTA 上传

---

## 6. 默认运行逻辑

设备的大致启动流程如下：

1. 上电
2. 初始化 NVS
3. 读取已保存配置
4. 启动传感器任务
5. 如果没有完整配置：
   - 进入 `SoftAP`
   - 启动网页配置页
6. 如果已有完整配置：
   - 先尝试连接 Wi-Fi
   - 连接成功后启动 MQTT
   - 启动网页控制台
7. 周期采样并发布数据
8. 如果 Wi-Fi 长时间离线：
   - 停止 MQTT
   - 切回 `SoftAP`
   - 保证设备还能重新配置

### 6.1 当前默认值

这些值来自 `Kconfig` 和代码默认值：

- 默认 `I2C port = 0`
- 默认 `UART port = 1`
- 默认发布周期 `10 秒`
- Wi-Fi 初始连接等待时间 `30000 ms`
- Wi-Fi 初始重试次数 `20`
- Wi-Fi 长时间离线后 AP 回退阈值 `180 秒`
- SoftAP SSID 前缀 `airmon`
- MQTT 默认端口 `1883`
- Discovery 默认前缀 `homeassistant`
- SCD41 `ASC` 默认开启
- SCD41 海拔补偿默认 `0 m`
- SCD41 温度偏移默认 `0.0`

---

## 7. 配置模型

当前保存到 NVS 的配置包括：

- 设备名称 `device_name`
- Wi-Fi 名称 `wifi_ssid`
- Wi-Fi 密码 `wifi_password`
- MQTT 主机 `mqtt_host`
- MQTT 端口 `mqtt_port`
- MQTT 用户名 `mqtt_username`
- MQTT 密码 `mqtt_password`
- Discovery 前缀 `discovery_prefix`
- Topic 根路径 `topic_root`
- 发布周期 `publish_interval_sec`
- SCD41 海拔补偿 `scd41_altitude_m`
- SCD41 温度偏移 `scd41_temp_offset_c`
- SCD41 ASC 开关 `scd41_asc_enabled`
- PMS 控制脚使能 `pms_control_pins_enabled`

### 7.1 默认生成方式

如果设备第一次启动没有配置，会自动生成：

- `device_name = air-monitor-<device_id>`
- `discovery_prefix = homeassistant`
- `topic_root = air_monitor/<device_id>`

其中 `device_id` 取自 ESP32 STA MAC 后 3 个字节的十六进制字符串。

---

## 8. Home Assistant 接入说明

## 8.1 Discovery 前缀

默认：

```text
homeassistant
```

如果你在网页里改了这个值，Discovery 会发到新的前缀下。

## 8.2 设备根主题

默认：

```text
air_monitor/<device_id>
```

在这个根路径下，当前固件会用到这些主题：

- `air_monitor/<device_id>/state`
- `air_monitor/<device_id>/diag`
- `air_monitor/<device_id>/availability`
- `air_monitor/<device_id>/cmd/restart`
- `air_monitor/<device_id>/cmd/factory_reset`
- `air_monitor/<device_id>/cmd/republish_discovery`
- `air_monitor/<device_id>/cmd/scd41_asc`
- `air_monitor/<device_id>/cmd/pms_sleep`
- `air_monitor/<device_id>/cmd/scd41_frc_reference_ppm`
- `air_monitor/<device_id>/cmd/apply_scd41_frc`

## 8.3 `state` 主题内容

`state` 是主要状态 JSON，当前会包含这些字段：

- `co2`
- `temperature`
- `humidity`
- `pm1_0`
- `pm2_5`
- `pm10_0`
- `particles_0_3um`
- `particles_0_5um`
- `particles_1_0um`
- `particles_2_5um`
- `particles_5_0um`
- `particles_10_0um`
- `pms_sleeping`
- `scd41_asc_enabled`
- `scd41_frc_reference_ppm`

## 8.4 `diag` 主题内容

`diag` 主题当前会包含：

- `wifi_rssi`
- `uptime_sec`
- `heap_free`
- `firmware_version`
- `last_error`

## 8.5 `availability` 主题

可用性主题只发：

- `online`
- `offline`

Home Assistant 会据此判断设备在线状态。

## 8.6 自动创建的实体

当前 Discovery 会创建以下实体。

### 传感器实体

- `CO2`
- `Temperature`
- `Humidity`
- `PM1.0`
- `PM2.5`
- `PM10`
- `Particles >0.3µm`
- `Particles >0.5µm`
- `Particles >1.0µm`
- `Particles >2.5µm`
- `Particles >5.0µm`
- `Particles >10µm`

### 诊断实体

- `Wi-Fi RSSI`
- `Uptime`
- `Heap Free`
- `Firmware Version`
- `Last Error`

### 开关实体

- `SCD41 ASC`
- `PMS7003 Sleep`

### 按钮实体

- `Restart`
- `Factory Reset`
- `Republish Discovery`
- `Apply SCD41 FRC`

### 数值实体

- `SCD41 FRC Reference`

---

## 9. MQTT 控制命令说明

### 9.1 按钮类命令

这些主题需要发送：

```text
PRESS
```

适用主题：

- `.../cmd/restart`
- `.../cmd/factory_reset`
- `.../cmd/republish_discovery`
- `.../cmd/apply_scd41_frc`

### 9.2 开关类命令

这些主题使用：

- `ON`
- `OFF`

适用主题：

- `.../cmd/scd41_asc`
- `.../cmd/pms_sleep`

### 9.3 数值类命令

FRC 参考值主题：

- `.../cmd/scd41_frc_reference_ppm`

可接受范围：

- `400 ~ 2000`

注意：

- 这个参考值目前是 **运行时值**
- 设备重启后会回到默认 `400`
- 真正执行 FRC 的动作，要再对 `apply_scd41_frc` 发送 `PRESS`

---

## 10. 网页控制台说明

网页控制台当前是单页界面，主要分成这些区域：

### 10.1 Status

显示：

- 当前是 `AP mode` 还是 `Station mode`
- `Device ID`
- 当前 `IP`
- `Wi-Fi RSSI`
- `MQTT` 是否已连上
- 固件版本
- `Last Error`

### 10.2 Telemetry

显示当前采样结果：

- `CO2`
- `Temperature`
- `Humidity`
- `PM2.5`
- `PM10`
- `PMS Sleep`

### 10.3 Wi-Fi & MQTT

允许填写：

- `Device Name`
- `Wi-Fi SSID`
- `Wi-Fi Password`
- `MQTT Host`
- `MQTT Port`
- `Publish Interval`
- `MQTT Username`
- `MQTT Password`
- `Discovery Prefix`
- `Topic Root`

点击 `Save Config` 后：

- 保存到 NVS
- 设备大约 `1.5 秒` 后自动重启

### 10.4 Sensor Controls

可操作：

- 修改 SCD41 海拔补偿
- 修改 SCD41 温度偏移
- 打开 ASC
- 关闭 ASC
- 唤醒 PMS7003
- 让 PMS7003 休眠
- 填写 FRC 参考值
- 执行 SCD41 强制校准
- 重新发布 Discovery

### 10.5 OTA Upload

网页支持直接上传固件二进制：

- 选择 `.bin`
- 点击上传
- 写入 OTA 分区
- 设置新的启动分区
- 自动重启

### 10.6 Device Actions

支持：

- 设备重启
- 恢复出厂设置

---

## 11. 网页接口（HTTP API）

如果你后面想自己做前端或脚本，也可以直接调当前的本地 API。

### 11.1 读取状态

```http
GET /api/status
```

返回：

- `diag`
- `snapshot`
- `config`
- `frc_reference_ppm`

### 11.2 保存配置

```http
POST /api/config
Content-Type: application/json
```

JSON 字段当前支持：

- `device_name`
- `wifi_ssid`
- `wifi_password`
- `mqtt_host`
- `mqtt_port`
- `mqtt_username`
- `mqtt_password`
- `discovery_prefix`
- `topic_root`
- `publish_interval_sec`
- `scd41_altitude_m`
- `scd41_temp_offset_c`

### 11.3 操作接口

```http
POST /api/action/restart
POST /api/action/factory-reset
POST /api/action/republish-discovery
POST /api/action/scd41-asc
POST /api/action/pms-sleep
POST /api/action/apply-frc
POST /api/ota
```

其中：

- `/api/action/scd41-asc` 需要 JSON：`{"enabled": true/false}`
- `/api/action/pms-sleep` 需要 JSON：`{"sleep": true/false}`
- `/api/action/apply-frc` 需要 JSON：`{"ppm": 400-2000}`
- `/api/ota` 直接 POST 固件二进制数据

---

## 12. 传感器采样逻辑说明

### 12.1 SCD41

当前代码逻辑：

- 初始化时停止周期测量
- 写入温度偏移
- 写入海拔补偿
- 写入 ASC 开关
- 再启动周期测量
- 运行中每秒查询一次是否有新数据
- 只要数据 ready，就读取 CO2 / 温度 / 湿度

### 12.2 PMS7003

当前代码逻辑：

- 使用 `UART1`
- 波特率 `9600`
- 连续接收数据帧
- 校验帧头和校验和
- 解析大气环境下的：
  - PM1.0
  - PM2.5
  - PM10
- 解析粒子计数：
  - `0.3um`
  - `0.5um`
  - `1.0um`
  - `2.5um`
  - `5.0um`
  - `10um`
- 保存最近 `6` 帧
- 计算平均值后作为当前有效值

### 12.3 PMS7003 预热逻辑

下面两种场景会触发预热：

- 设备刚启动
- 从休眠唤醒 PMS7003

触发后：

- 固件会等待 `30 秒`
- 这段时间即便收到串口帧，也不会作为有效 PM 数据

这和你给的 PMS7003 资料是一致的。

---

## 13. AP / STA 行为说明

### 13.1 首次上电

如果设备没有完整配置：

- 自动进入 SoftAP
- AP 名称格式：

```text
airmon-<MAC后3字节>
```

当前代码中 AP 是开放网络，没有密码。

连接后访问：

```text
http://192.168.4.1/
```

### 13.2 已配置设备

如果设备已有 Wi-Fi 和 MQTT 配置：

- 尝试连接到保存的 Wi-Fi
- 成功后启动 MQTT
- 同时保持本地网页可用

### 13.3 离线回退

如果设备处于 STA 模式，但 Wi-Fi 长时间断开：

- 当前阈值是 `180 秒`
- 设备会停止 MQTT
- 切回 AP 模式
- 启动网页控制台，方便重新配置

---

## 14. 编译环境

当前工程按下面环境验证过：

- `ESP-IDF v5.5.3`
- 目标芯片：`ESP32-S3`
- 分区表：自定义 `partitions.csv`
- Flash size：当前配置成 `4MB`

### 14.1 当前分区表

当前分区表为：

- `nvs`
- `otadata`
- `phy_init`
- `ota_0`
- `ota_1`

应用分区大小：

- `ota_0 = 0x180000`
- `ota_1 = 0x180000`

也就是说当前工程已经按 OTA 双分区方式配置。

---

## 15. 关于目录名尾随空格的问题

你的工程目录现在的真实路径是：

```text
/Users/yifan/Documents/Vscode/particulate_matter_CO2_sensor␠
```

注意最后真的有一个空格。

这会导致 `idf.py` 的路径一致性检查出现异常，表现为：

- `idf.py build` 配置完成后又报 source path mismatch

### 15.1 当前可用的解决方式

最稳妥的方法是先建立一个没有尾随空格的软链接，再从软链接路径编译：

```bash
ln -s "/Users/yifan/Documents/Vscode/particulate_matter_CO2_sensor " /tmp/airmon-src
```

然后：

```bash
. /Users/yifan/.espressif/v5.5.3/esp-idf/export.sh
cd /tmp/airmon-src
idf.py -B /tmp/airmon-build build
```

### 15.2 当前编译产物

目前已经成功生成：

- `/tmp/airmon-build/air_monitor.bin`

镜像大小约：

- `0xdf890`

相对于单个 `0x180000` OTA 分区，仍有较充足余量。

---

## 16. 编译步骤

### 16.1 初始化环境

```bash
ln -s "/Users/yifan/Documents/Vscode/particulate_matter_CO2_sensor " /tmp/airmon-src
. /Users/yifan/.espressif/v5.5.3/esp-idf/export.sh
cd /tmp/airmon-src
```

### 16.2 编译

```bash
idf.py -B /tmp/airmon-build build
```

如果你还是碰到路径校验问题，可以直接用 Ninja：

```bash
/Users/yifan/.espressif/tools/ninja/1.12.1/ninja -C /tmp/airmon-build
```

---

## 17. 烧录步骤

### 17.1 查看串口设备

macOS 下常见：

```bash
ls /dev/cu.usb*
```

### 17.2 烧录并打开串口监视

```bash
. /Users/yifan/.espressif/v5.5.3/esp-idf/export.sh
cd /tmp/airmon-src
idf.py -B /tmp/airmon-build -p /dev/cu.usbmodemXXXX flash monitor
```

把 `/dev/cu.usbmodemXXXX` 换成你电脑上实际的串口号。

---

## 18. 第一次上电怎么操作

建议按下面顺序：

1. 先检查接线是否全部正确
2. 尤其确认：
   - PMS7003 电源是 `5V`
   - PMS7003 串口和控制脚是 `3.3V TTL`
   - 所有 GND 共地
3. 烧录固件
4. 上电
5. 如果设备没有已保存配置：
   - 搜索 Wi-Fi
   - 找到 `airmon-xxxxxx`
   - 连接它
   - 打开 `192.168.4.1`
6. 在网页里填写：
   - 你的家中 Wi-Fi
   - MQTT Broker 地址
   - MQTT 端口
   - MQTT 用户名密码
   - 是否修改设备名、Discovery 前缀、Topic Root
7. 点击保存
8. 设备自动重启
9. 设备连上家中 Wi-Fi
10. Home Assistant 自动发现实体

---

## 19. Home Assistant 联调建议

建议先确认以下顺序：

### 19.1 先看串口日志

确认：

- Wi-Fi 是否连接成功
- MQTT 是否连接成功
- 是否有传感器初始化报错

### 19.2 再看 MQTT Broker

确认这些主题是否已经出现：

- `air_monitor/<device_id>/availability`
- `air_monitor/<device_id>/state`
- `air_monitor/<device_id>/diag`

### 19.3 最后看 Home Assistant

确认：

- MQTT 集成已启用
- Broker 地址正确
- 自动发现已启用
- 新设备是否出现

如果 Discovery 没有刷新，可以在网页点：

- `Republish Discovery`

---

## 20. 当前代码里值得知道的实现细节

### 20.1 SCD41 ASC 开关会持久化

如果你通过网页或 MQTT 切换 `ASC`：

- 会同步更新运行时状态
- 会写回当前配置
- 设备重启后继续保持这个设置

### 20.2 FRC 参考值目前不持久化

`SCD41 FRC Reference` 目前是运行时值：

- 默认从 `400 ppm` 开始
- 你可以通过网页或 MQTT 改它
- 但设备重启后会重新回到默认值

### 20.3 恢复出厂设置做了什么

当前恢复出厂会：

- 擦除 NVS 中保存的配置 key
- 重启设备

恢复后设备会重新进入 AP 配置模式。

### 20.4 Wi-Fi 正常但 MQTT 配错时怎么办

如果：

- 设备成功连上 Wi-Fi
- 但 MQTT 参数填错

那设备仍然会在局域网里提供网页控制台。

你可以：

- 查出它的 IP
- 打开网页
- 重新修改 MQTT 配置

---

## 21. 已知限制

当前版本已经可编译、可作为上板验证基础，但仍有一些你应该知道的点：

1. 当前网页控制台 **不带认证**
   - 这是按你要求实现的
   - 仅适合受信任内网

2. 当前目录名尾随空格会影响 `idf.py`
   - 已在文档中给出规避方法

3. FRC 是专业操作
   - 只有在已知稳定参考浓度环境下才应该执行
   - 随便写数值并执行，会让 SCD41 校准变差

4. PMS7003 休眠再唤醒后需要等 `30 秒`
   - 这是传感器特性，不是程序慢

5. 当前虽然已经完成完整编译验证，但是否与你手上的实际模块、电源、线材完全兼容，仍需要实机联调确认

---

## 22. 常见问题排查

### Q1：PMS7003 一直没有数据

先检查：

- 是否真的给了 `5V`
- `TX/RX` 是否交叉
- 是否共地
- 是否刚唤醒，还处在 30 秒预热期
- `SET` 是否被意外拉低

### Q2：SCD41 没有读数

先检查：

- 是否接到 `3.3V`
- `SDA/SCL` 是否接反
- I2C 模块是否确实是可直接接线的成品模块

### Q3：Home Assistant 没有自动发现

检查：

- 设备是否连上 Wi-Fi
- MQTT 是否连上
- Discovery 前缀是否还是 `homeassistant`
- Home Assistant 的 MQTT 集成是否启用了自动发现

### Q4：网页进不去

如果是首次上电：

- 连接设备 AP
- 打开 `192.168.4.1`

如果已经配置好 Wi-Fi：

- 去路由器 DHCP 列表找设备 IP
- 再访问它

### Q5：保存配置后为什么会重启

这是当前设计的一部分：

- 保存配置后会写入 NVS
- 然后延迟约 `1.5 秒` 自动重启
- 这样可以重新按新配置启动 Wi-Fi 和 MQTT

---

## 23. 关键源码位置

如果你后续想自己改代码，建议优先看这些文件：

- `main/app_main.c`
  - 整个设备启动和调度逻辑

- `components/platform/platform_config.c`
  - NVS 配置保存与读取

- `components/platform/platform_wifi.c`
  - AP/STA 切换

- `components/sensors/scd41.c`
  - SCD41 指令与读数

- `components/sensors/pms7003.c`
  - PMS7003 串口协议解析

- `components/sensors/sensors.c`
  - 传感器任务、平滑、预热、控制逻辑

- `components/mqtt_ha/mqtt_ha.c`
  - MQTT Discovery、状态发布、控制命令

- `components/provisioning_web/provisioning_web.c`
  - 本地网页控制台和 OTA

---

## 24. 你现在最实用的上手建议

如果你准备马上做硬件联调，我建议按这个顺序：

1. 按本文接线表接好所有线
2. 先不要省略 `SET` 和 `RESET`
3. 先只验证串口日志里：
   - SCD41 初始化
   - PMS7003 串口收帧
4. 再验证 AP 页面
5. 再填 Wi-Fi / MQTT
6. 最后再看 Home Assistant Discovery

这样排错最快。

---

## 25. 当前状态总结

截至现在，这个工程已经不是“空壳”或“草图”，而是一个已经具备以下条件的可用固件骨架：

- 能编译
- 有双 OTA 分区
- 有网页配置和 OTA
- 有传感器采集
- 有 MQTT Discovery
- 有 HA 控制实体
- 有 AP 回退机制

下一步最值得做的事情通常有两个：

- 实机接线联调
- 根据你实际外壳/电源/布线情况微调 GPIO 和供电方式

如果你愿意，我下一步还可以继续帮你补一份：

- “上电调试 checklist”
- 或者“Home Assistant 里每个实体的用途说明”
- 或者“基于你现在这块 YD-ESP32-S3 的实物接线图文字版/表格版”
