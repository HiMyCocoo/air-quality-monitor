const configFields = ['mqtt_url', 'scd41_altitude_m', 'scd41_temp_offset_c'];
    const configInputs = Object.fromEntries(configFields.map((id) => [id, document.getElementById(id)]));
    const overviewNetworkEl = document.getElementById('overviewNetwork');
    const overviewModulesEl = document.getElementById('overviewModules');
    const overviewSystemEl = document.getElementById('overviewSystem');
    const airSummaryEl = document.getElementById('airSummary');
    const airMetricsPrimaryEl = document.getElementById('airMetricsPrimary');
    const airMetricsSecondaryEl = document.getElementById('airMetricsSecondary');
    const particleSummaryEl = document.getElementById('particleSummary');
    const particleMetricsEl = document.getElementById('particleMetrics');
    const frcInput = document.getElementById('frc_reference_ppm');
    const firmwareInput = document.getElementById('firmware');
    const firmwareNameEl = document.getElementById('firmware_name');
    const firmwareVersionTextEl = document.getElementById('firmware_version_text');
    const githubOtaSectionEl = document.getElementById('github_ota_section');
    const githubLatestVersionTextEl = document.getElementById('github_latest_version_text');
    const githubRepoTextEl = document.getElementById('github_repo_text');
    const githubOtaStatusTextEl = document.getElementById('github_ota_status_text');
    const githubOtaMetaEl = document.getElementById('github_ota_meta');
    const githubOtaCheckBtn = document.getElementById('github_ota_check_btn');
    const githubOtaUpdateBtn = document.getElementById('github_ota_update_btn');
    const githubOtaProgressEl = document.getElementById('github_ota_progress');
    const githubOtaProgressLabelEl = document.getElementById('github_ota_progress_label');
    const githubOtaProgressPercentEl = document.getElementById('github_ota_progress_percent');
    const githubOtaProgressFillEl = document.getElementById('github_ota_progress_fill');
    const otaProgressEl = document.getElementById('ota_progress');
    const otaProgressLabelEl = document.getElementById('ota_progress_label');
    const otaProgressPercentEl = document.getElementById('ota_progress_percent');
    const otaProgressFillEl = document.getElementById('ota_progress_fill');
    const otaUploadBtn = document.getElementById('ota_upload_btn');
    const mqttPrivacyHintEl = document.getElementById('mqtt_privacy_hint');
    const ascToggle = document.getElementById('asc_toggle');
    const ascStatusText = document.getElementById('asc_status_text');
    const sps30Toggle = document.getElementById('sps30_toggle');
    const sps30StatusText = document.getElementById('sps30_status_text');
    const sps30FanCleaningBtn = document.getElementById('sps30_fan_cleaning_btn');
    const statusLedToggle = document.getElementById('status_led_toggle');
    const statusLedText = document.getElementById('status_led_text');
    const frcApplyBtn = document.getElementById('frc_apply_btn');
    const republishBtn = document.getElementById('republish_btn');
    const tabButtons = [...document.querySelectorAll('.nav-btn')];
    const tabPanels = [...document.querySelectorAll('.tab-panel')];
    const languageButtons = [...document.querySelectorAll('.lang-btn')];

    let configDirty = false;
    let mqttUrlDirty = false;
    let otaBusy = false;
    let otaGithubCheckInFlight = false;
    let otaGithubLastAutoCheckAt = 0;
    let otaGithubKnownLatestVersion = '';
    let otaGithubNotifiedVersion = '';
    let latestStatusData = null;
    const activeTabStorageKey = 'airmon-active-tab';
    const languageStorageKey = 'airmon-language';
    const supportedLanguages = ['zh-CN', 'en'];
    const githubReleaseAutoRefreshMs = 2 * 60 * 1000;

    const translations = {
      'zh-CN': {
        'pageTitle': '空气监测节点后台',
        'nav.sectionAria': '设备后台功能分区',
        'nav.realtime': '实时监测',
        'nav.config': '项目配置',
        'nav.maintenance': '维护操作',
        'nav.languageAria': '语言切换',
        'nav.langZh': '简中',
        'nav.langEn': 'EN',
        'realtime.airOverviewTitle': '空气质量总览',
        'realtime.particleProfileTitle': '颗粒物画像',
        'realtime.runtimeOverviewTitle': '运行概览',
        'realtime.networkStatusTitle': '联网状态',
        'realtime.moduleStatusTitle': '模块状态',
        'realtime.systemSamplingTitle': '系统与采样',
        'config.mqtt.title': 'MQTT 接入',
        'config.mqtt.description': '配置设备接入 Home Assistant 或外部 MQTT Broker 的连接参数。',
        'config.mqtt.urlLabel': 'MQTT URL',
        'config.mqtt.urlHint': '格式：mqtt://[user:password@]host[:port]。用户密码中的特殊字符需 URL 编码。',
        'config.mqtt.footer': '填入后点击保存会自动尝试连接并发布 Discovery 机制。',
        'config.mqtt.saveButton': '保存配置',
        'config.sensor.title': '传感器策略',
        'config.sensor.description': '调整核心环境传感器的采样行为和基准补偿参数。',
        'config.sensor.scd41Title': 'SCD41 补偿参数',
        'config.sensor.altitudeLabel': '海拔补偿（米）',
        'config.sensor.altitudePlaceholder': '0 - 3000',
        'config.sensor.tempOffsetLabel': '温度偏移（°C）',
        'config.sensor.tempOffsetPlaceholder': '例如: 2.5',
        'config.sensor.ascTitle': 'ASC 自校准',
        'config.sensor.ascToggleTitle': '自动自校准',
        'config.sensor.ascToggleAria': 'SCD41 ASC 自校准开关',
        'config.sensor.footer': '修改策略后，系统可能需要短暂重置传感器实例以应用新参数。',
        'config.sensor.applyButton': '应用参数',
        'maintenance.switches.title': '外设状态开关',
        'maintenance.switches.description': '控制系统特定模块的开关状态。此操作实时生效，无须额外保存。',
        'maintenance.switches.sps30Title': 'SPS30 连续采样',
        'maintenance.switches.sps30Aria': 'SPS30 工作状态开关',
        'maintenance.switches.rgbTitle': 'RGB 状态灯',
        'maintenance.switches.rgbAria': 'RGB 状态灯开关',
        'maintenance.tools.title': '校准与调试工具',
        'maintenance.tools.description': '强制触发环境传感器校准，或从 Home Assistant 重新同步实体。',
        'maintenance.tools.discoveryTitle': '重发 Discovery',
        'maintenance.tools.discoveryDesc': '向 MQTT Broker 重新推送 Home Assistant 发现消息谱。',
        'maintenance.tools.discoveryButton': '推送 Discovery 配置',
        'maintenance.tools.fanCleaningTitle': 'SPS30 自动风扇清洁',
        'maintenance.tools.fanCleaningDesc': '手动触发 SPS30 内置自动风扇清洁流程。仅在传感器运行中可用，执行约 10 秒，期间 PM 数据会短暂失效。',
        'maintenance.tools.fanCleaningButton': '执行风扇清洁',
        'maintenance.tools.frcTitle': 'FRC 强制校准',
        'maintenance.tools.frcLabel': 'FRC 参考值 (ppm)',
        'maintenance.tools.frcButton': '执行 FRC 校准',
        'ota.title': '固件升级 (OTA)',
        'ota.description': '在线检查 GitHub Releases 自动更新，或直接手动上传本地固件包。',
        'ota.currentVersionLabel': '当前版本',
        'ota.githubTitle': 'GitHub 自动更新',
        'ota.latestVersionLabel': '最新版本',
        'ota.statusLabel': '更新状态',
        'ota.idleState': '等待检查',
        'ota.autoCheckHint': '页面加载后会自动检查一次 GitHub 最新 Release，并每 2 分钟自动刷新。',
        'ota.checkButton': '检查更新',
        'ota.updateButton': '直接升级',
        'ota.manualTitle': '本地手动上传',
        'ota.manualDesc': '上传并校验本地保存的 .bin 固件包',
        'ota.noFileSelectedAction': '点击选取 .bin 固件包',
        'ota.dropzoneHint': '大小限制视设备分区而定，自动隐式校验',
        'ota.waitingUpload': '等待上传',
        'ota.uploadButton': '开始上传并升级',
        'ota.warningHtml': '<strong>OTA 升级须知：</strong>整个校验与写入过程大约需要 30-60 秒，完成后设备将自动重启。<br>升级期间<strong>请勿断开电源或刷新页面</strong>。',
        'danger.title': '危险操作区 (Danger Zone)',
        'danger.description': '请谨慎操作，恢复出厂设置将抹除一切网络配置与补偿信息并重启至配网模式。',
        'danger.footer': '恢复后将清除 Wi-Fi、MQTT 与传感器补偿配置，并回到 BLE 配网模式。',
        'danger.restartOnly': '仅重新启动',
        'danger.factoryReset': '恢复出厂设置',
        'status.currentPlaceholder': '当前：--',
        'status.currentLabel': '当前：{value}',
        'common.notProvided': '未提供',
        'common.assessmentUnavailable': '评估不可用',
        'common.listSeparator': '，',
        'common.none': '无',
        'common.offline': '离线',
        'common.online': '在线',
        'common.connected': '已连接',
        'common.disconnected': '未连接',
        'common.unconfigured': '未配置',
        'common.unavailable': '暂不可用',
        'common.connectedRssi': '已连接 ({rssi} dBm)',
        'common.bleProvisioning': 'BLE 配网',
        'common.lanMode': '局域网运行',
        'common.enabled': '开启',
        'common.disabled': '关闭',
        'common.running': '运行中',
        'common.sleeping': '休眠中',
        'common.connectedOnly': '已连接',
        'time.sec': '秒',
        'time.min': '分',
        'time.hour': '小时',
        'particle.support.co2High': 'CO2 {value} ppm，说明新风交换偏弱。',
        'particle.support.co2Low': 'CO2 {value} ppm，说明通风暂时不是主要矛盾。',
        'particle.support.rhLow': '湿度 {value}% 偏低，灰尘更容易被走动和清扫重新扬起。',
        'particle.support.rhHighFine': '湿度 {value}% 偏高，细颗粒会更闷，也更容易吸湿变大。',
        'particle.support.tempWarm': '温度 {value}°C 偏高，体感会更闷。',
        'particle.support.tempCool': '温度 {value}°C 已经偏低，通风时注意别让房间继续变冷。',
        'particle.insightTitle': '空气状况分析',
        'particle.simpleScene.background': '空气良好，颗粒物处于日常室内背景水平，无需担心。',
        'particle.simpleScene.fine-source': '细颗粒偏高，可能来自烹饪油烟、燃烧或室外污染输入。',
        'particle.simpleScene.fine-stale': '细颗粒正在室内累积，通风可能不足。',
        'particle.simpleScene.coarse-dust': '灰尘或粗颗粒偏高，可能与走动、清扫或室外扬尘有关。',
        'particle.simpleScene.mixed-activity': '细颗粒和粗颗粒同时偏高，室内活动与室外输入共同影响。',
        'particle.simpleScene.unavailable': '正在采集数据，稍后即可查看分析。',
        'particle.simpleGuidance.background': '保持日常通风即可，无需额外处理。',
        'particle.simpleGuidance.fine-source': '开启油烟机或净化器，减少污染源。',
        'particle.simpleGuidance.fine-stale': '建议短时通风换气，排出累积的颗粒和 CO₂。',
        'particle.simpleGuidance.coarse-dust': '建议湿拖或 HEPA 吸尘，避免干扫扬尘。',
        'particle.simpleGuidance.mixed-activity': '先短时通风，再清洁地面减少二次扬尘。',
        'particle.simpleGuidance.unavailable': '等待数据稳定后再做处理。',
        'wifi.waitingProvisioning': '等待配网',
        'co2Comp.bmp390': 'BMP390 动态气压',
        'co2Comp.altitude': 'SCD41 海拔补偿',
        'co2Comp.inactive': '未生效',
        'co2Comp.offline': 'SCD41 离线',
        'sgp41.status.warmup': '预热中',
        'sgp41.status.vocReady': 'VOC 就绪 / NOx 学习中',
        'sgp41.status.noxReady': 'NOx 就绪 / VOC 学习中',
        'sgp41.status.learning': '学习中',
        'sgp41.meta.offline': 'SGP41 离线',
        'sgp41.meta.warmup': '预热中，随后进入算法学习',
        'sgp41.meta.learningRemaining': '算法学习中，预计还需 {remaining}',
        'sgp41.meta.waitingSamples': '等待稳定样本',
        'pressureTrend.offline': 'BMP390 离线',
        'pressureTrend.waitingSample': '等待有效气压样本',
        'pressureTrend.waitingHistory': '需要约 1 小时连续气压历史',
        'pressureTrend.meta': '过去 {span} 分钟折算为 3 小时变化 {delta} hPa',
        'pressureTrend.summary': '{label}，基于过去 {span} 分钟',
        'rainOutlook.season': '杭州{season}',
        'rainOutlook.seasonNeutral': '未校时，季节按中性处理',
        'rainOutlook.pressure': '气压 {delta} hPa/3h',
        'rainOutlook.humidityTrend': '湿度 {delta} %RH/3h',
        'rainOutlook.currentHumidity': '当前湿度 {value} %RH',
        'rainOutlook.humidityUnavailable': '湿度趋势暂不可用',
        'rainOutlook.dewPointSpread': '露点差 {value} °C',
        'ota.authSavedHint': '已保存 MQTT 认证信息。出于安全原因，页面不会回显用户名或密码；如需修改，请重新输入完整 MQTT URL。',
        'ota.noFileSelected': '尚未选择任何文件',
        'ota.noGithubAutoUpdate': '当前固件未启用 GitHub 自动更新，仍可继续使用手动上传 OTA。',
        'ota.meta.currentLatest': '当前 {current}，最新 {latest}',
        'ota.meta.latestVersion': 'GitHub 最新版本 {latest}',
        'ota.meta.asset': '升级文件 {asset}',
        'ota.meta.autoRefresh': '页面加载后会自动检查一次 GitHub 最新 Release，并每 2 分钟自动刷新。',
        'ota.meta.alreadyLatest': '当前已经是 GitHub 最新版本',
        'ota.notice.autoNewVersion': '检测到新的 GitHub 固件 {version}，页面已自动刷新，可直接升级',
        'ota.checkButtonBusy': '查询中...',
        'ota.updateButtonBusy': '下载中...',
        'ota.updateButtonDirect': '从 GitHub 直接升级',
        'ota.uploadButtonBusy': '上传中...',
        'ota.uploadButtonProcessing': '处理中...',
        'toggle.asc.unavailable': 'SCD41 不可用',
        'toggle.sps30.unavailable': 'SPS30 不可用',
        'toggle.rgb.unavailable': 'RGB 灯不可用',
        'overview.mode': '运行模式',
        'overview.wifi': 'Wi-Fi',
        'overview.mqtt': 'MQTT',
        'overview.ip': '内网 IP',
        'overview.deviceId': '设备标识',
        'overview.firmware': '固件版本',
        'overview.co2Compensation': 'CO2 补偿',
        'overview.uptime': '运行时长',
        'overview.lastError': '最近错误',
        'summary.overall': '综合空气评估：{value}',
        'metrics.pmAqi': 'PM AQI 估算',
        'metrics.co2': 'CO2 浓度',
        'metrics.indoorTemp': '室内温度',
        'metrics.relHumidity': '相对湿度',
        'metrics.pressure': '当前气压',
        'metrics.pressureTrend': '3 小时气压趋势',
        'metrics.rainChance': '短时降雨可能',
        'metrics.vocIndex': 'VOC 指数 (Sensirion)',
        'metrics.noxIndex': 'NOx 指数 (Sensirion)',
        'metrics.scd41Unavailable': 'SCD41 暂不可用',
        'particle.section.mass': 'PM 质量浓度',
        'particle.section.count': '颗粒数',
        'particle.pm1Meta': '超细到细颗粒质量浓度',
        'particle.pm25Meta': '最常用的细颗粒指标',
        'particle.pm4Meta': '介于 PM2.5 和 PM10 之间',
        'particle.pm10Meta': '可吸入颗粒物总量',
        'particle.count05Meta': '0.5 微米以内的总体颗粒数',
        'particle.count10Meta': '1.0 微米以内的总体颗粒数',
        'particle.count25Meta': '2.5 微米以内的总体颗粒数',
        'particle.count40Meta': '4.0 微米以内的总体颗粒数',
        'particle.count100Meta': '10.0 微米以内的总体颗粒数',
        'action.configSaveFailed': '配置保存失败',
        'action.configSavedRestartRuntime': 'MQTT 配置已保存，系统将重启以重新连接服务端。SCD41 补偿参数已即时下发。',
        'action.configSavedRestart': 'MQTT 配置已保存，系统将重启以重新连接服务端。',
        'action.runtimeApplied': 'SCD41 补偿参数已即时生效。',
        'action.configSaved': '配置已保存。',
        'action.operationFailed': '操作失败',
        'action.ascEnabled': 'ASC 已开启',
        'action.ascDisabled': 'ASC 已关闭',
        'action.sps30Sleeping': 'SPS30 休眠中',
        'action.sps30Waking': 'SPS30 唤醒中',
        'action.ledEnabled': '指示灯开启',
        'action.ledDisabled': '指示灯关闭',
        'action.fanCleaningFailed': '风扇清洁启动失败',
        'action.fanCleaningStarted': 'SPS30 风扇清洁已启动，约 10 秒内 PM 数据会短暂失效',
        'action.frcFailed': 'FRC 校准失败',
        'action.frcDone': '已强制校准',
        'action.requestFailed': '请求失败',
        'action.discoveryRepublished': '已重新发送 Discovery 报文',
        'action.restartRequested': '已请求重启',
        'action.factoryResetConfirm': '⚠️ 此操作将清除所有 Wi-Fi 和 MQTT 配置，是否继续？',
        'action.factoryResetFailed': '失败',
        'action.factoryResetDone': '设备即将初始化完毕',
        'action.githubCheckFailed': '检查 GitHub 最新版本失败',
        'action.githubNewVersion': '检测到新版本 {version}，可直接升级',
        'action.githubAlreadyLatest': '当前已经是 GitHub 最新版本',
        'action.githubUpdateConfirm': '设备将从 GitHub 下载{version} OTA 固件并自动重启，是否继续？',
        'action.githubLatestVersionSuffix': ' 最新版本',
        'action.githubUpdateStartFailed': 'GitHub OTA 启动失败',
        'action.githubUpdateStarted': '已开始从 GitHub 下载 OTA 固件，请勿断电。',
        'action.selectFirmware': '请选择固件',
        'action.uploadingFirmware': '正在上传固件...',
        'action.uploadComplete': '上传完成，正在校验并切换固件...',
        'action.uploadFailed': '上传失败',
        'action.uploadNetworkInterrupted': 'OTA 升级失败: 网络连接已中断',
        'action.uploadValidated': '固件校验完成，设备即将重启',
        'action.uploadSuccess': '固件上传及校验成功，即将重启！',
        'action.uploadRetry': '升级失败，请重试'
      },
      en: {
        'pageTitle': 'Air Monitor Console',
        'nav.sectionAria': 'Console sections',
        'nav.realtime': 'Live Monitor',
        'nav.config': 'Configuration',
        'nav.maintenance': 'Maintenance',
        'nav.languageAria': 'Language switcher',
        'nav.langZh': '简中',
        'nav.langEn': 'EN',
        'realtime.airOverviewTitle': 'Air Quality Overview',
        'realtime.particleProfileTitle': 'Particle Profile',
        'realtime.runtimeOverviewTitle': 'Runtime Overview',
        'realtime.networkStatusTitle': 'Network Status',
        'realtime.moduleStatusTitle': 'Module Status',
        'realtime.systemSamplingTitle': 'System and Sampling',
        'config.mqtt.title': 'MQTT Access',
        'config.mqtt.description': 'Configure connectivity for Home Assistant or any external MQTT broker.',
        'config.mqtt.urlLabel': 'MQTT URL',
        'config.mqtt.urlHint': 'Format: mqtt://[user:password@]host[:port]. URL-encode special characters in usernames or passwords.',
        'config.mqtt.footer': 'Saving will immediately try the connection and publish MQTT Discovery payloads.',
        'config.mqtt.saveButton': 'Save Configuration',
        'config.sensor.title': 'Sensor Strategy',
        'config.sensor.description': 'Tune sampling behavior and compensation parameters for core environmental sensors.',
        'config.sensor.scd41Title': 'SCD41 Compensation',
        'config.sensor.altitudeLabel': 'Altitude Compensation (m)',
        'config.sensor.altitudePlaceholder': '0 - 3000',
        'config.sensor.tempOffsetLabel': 'Temperature Offset (°C)',
        'config.sensor.tempOffsetPlaceholder': 'Example: 2.5',
        'config.sensor.ascTitle': 'ASC Self-Calibration',
        'config.sensor.ascToggleTitle': 'Automatic self-calibration',
        'config.sensor.ascToggleAria': 'SCD41 ASC self-calibration switch',
        'config.sensor.footer': 'Applying these settings may briefly reset the sensor instance so the new parameters take effect.',
        'config.sensor.applyButton': 'Apply Parameters',
        'maintenance.switches.title': 'Peripheral Switches',
        'maintenance.switches.description': 'Control the live state of selected system modules. Changes take effect immediately and do not require an extra save.',
        'maintenance.switches.sps30Title': 'SPS30 Continuous Sampling',
        'maintenance.switches.sps30Aria': 'SPS30 operating state switch',
        'maintenance.switches.rgbTitle': 'RGB Status LED',
        'maintenance.switches.rgbAria': 'RGB status LED switch',
        'maintenance.tools.title': 'Calibration and Debug Tools',
        'maintenance.tools.description': 'Force sensor calibration tasks or resync entities from Home Assistant.',
        'maintenance.tools.discoveryTitle': 'Republish Discovery',
        'maintenance.tools.discoveryDesc': 'Send Home Assistant discovery payloads to the MQTT broker again.',
        'maintenance.tools.discoveryButton': 'Republish Discovery',
        'maintenance.tools.fanCleaningTitle': 'SPS30 Fan Cleaning',
        'maintenance.tools.fanCleaningDesc': 'Manually trigger the built-in SPS30 fan-cleaning routine. It only works while the sensor is running, takes about 10 seconds, and briefly invalidates PM data.',
        'maintenance.tools.fanCleaningButton': 'Start Fan Cleaning',
        'maintenance.tools.frcTitle': 'Forced FRC Calibration',
        'maintenance.tools.frcLabel': 'FRC Reference (ppm)',
        'maintenance.tools.frcButton': 'Apply FRC Calibration',
        'ota.title': 'Firmware Update (OTA)',
        'ota.description': 'Check GitHub Releases for updates or upload a local firmware package directly.',
        'ota.currentVersionLabel': 'Current Version',
        'ota.githubTitle': 'GitHub Auto Update',
        'ota.latestVersionLabel': 'Latest Version',
        'ota.statusLabel': 'Update Status',
        'ota.idleState': 'Waiting to Check',
        'ota.autoCheckHint': 'The page checks the latest GitHub release once on load and refreshes automatically every 2 minutes.',
        'ota.checkButton': 'Check for Updates',
        'ota.updateButton': 'Update Now',
        'ota.manualTitle': 'Local Upload',
        'ota.manualDesc': 'Upload and validate a locally stored .bin firmware image',
        'ota.noFileSelectedAction': 'Click to choose a .bin firmware file',
        'ota.dropzoneHint': 'Maximum size depends on the device partition layout. Validation is automatic.',
        'ota.waitingUpload': 'Waiting to Upload',
        'ota.uploadButton': 'Upload and Update',
        'ota.warningHtml': '<strong>OTA reminder:</strong> Validation and flashing usually take 30 to 60 seconds, then the device reboots automatically.<br><strong>Do not cut power or refresh this page</strong> during the update.',
        'danger.title': 'Danger Zone',
        'danger.description': 'Use with care. A factory reset erases all network settings and compensation data, then reboots into provisioning mode.',
        'danger.footer': 'Resetting clears Wi-Fi, MQTT, and sensor compensation settings, then returns the device to BLE provisioning mode.',
        'danger.restartOnly': 'Restart Only',
        'danger.factoryReset': 'Factory Reset',
        'status.currentPlaceholder': 'Current: --',
        'status.currentLabel': 'Current: {value}',
        'common.notProvided': 'Unavailable',
        'common.assessmentUnavailable': 'Assessment unavailable',
        'common.listSeparator': ', ',
        'common.none': 'None',
        'common.offline': 'Offline',
        'common.online': 'Online',
        'common.connected': 'Connected',
        'common.disconnected': 'Disconnected',
        'common.unconfigured': 'Not configured',
        'common.unavailable': 'Unavailable',
        'common.connectedRssi': 'Connected ({rssi} dBm)',
        'common.bleProvisioning': 'BLE provisioning',
        'common.lanMode': 'LAN mode',
        'common.enabled': 'Enabled',
        'common.disabled': 'Disabled',
        'common.running': 'Running',
        'common.sleeping': 'Sleeping',
        'common.connectedOnly': 'Connected',
        'time.sec': 'sec',
        'time.min': 'min',
        'time.hour': 'hr',
        'particle.support.co2High': 'CO2 {value} ppm also points to weak fresh-air exchange.',
        'particle.support.co2Low': 'CO2 {value} ppm suggests ventilation is not the main issue right now.',
        'particle.support.rhLow': 'Humidity {value}% is low enough for dust to resuspend more easily.',
        'particle.support.rhHighFine': 'Humidity {value}% is high enough to make fine aerosols feel muggy and grow more easily.',
        'particle.support.tempWarm': 'Temperature {value}°C makes the room feel warmer and stuffier.',
        'particle.support.tempCool': 'Temperature {value}°C is already on the cool side, so avoid over-ventilating for too long.',
        'particle.insightTitle': 'Air Quality Analysis',
        'particle.simpleScene.background': 'Air is clean. Particles are at normal indoor background levels.',
        'particle.simpleScene.fine-source': 'Fine particles are elevated, likely from cooking, combustion, or outdoor pollution.',
        'particle.simpleScene.fine-stale': 'Fine particles are building up indoors due to limited fresh air exchange.',
        'particle.simpleScene.coarse-dust': 'Dust or coarse particles are elevated, likely from movement, cleaning, or outdoor sources.',
        'particle.simpleScene.mixed-activity': 'Both fine and coarse particles are elevated from indoor activity and outdoor input.',
        'particle.simpleScene.unavailable': 'Collecting data. Analysis will be available shortly.',
        'particle.simpleGuidance.background': 'Maintain routine ventilation. No extra action needed.',
        'particle.simpleGuidance.fine-source': 'Run the range hood or air purifier to reduce the source.',
        'particle.simpleGuidance.fine-stale': 'Open windows briefly to flush out accumulated particles and CO₂.',
        'particle.simpleGuidance.coarse-dust': 'Use damp mopping or HEPA vacuum instead of dry sweeping.',
        'particle.simpleGuidance.mixed-activity': 'Ventilate briefly, then clean floors and surfaces.',
        'particle.simpleGuidance.unavailable': 'Wait for stable data before taking action.',
        'wifi.waitingProvisioning': 'Waiting for provisioning',
        'co2Comp.bmp390': 'BMP390 dynamic pressure',
        'co2Comp.altitude': 'SCD41 altitude compensation',
        'co2Comp.inactive': 'Not applied',
        'co2Comp.offline': 'SCD41 offline',
        'sgp41.status.warmup': 'Warming up',
        'sgp41.status.vocReady': 'VOC ready / NOx learning',
        'sgp41.status.noxReady': 'NOx ready / VOC learning',
        'sgp41.status.learning': 'Learning',
        'sgp41.meta.offline': 'SGP41 offline',
        'sgp41.meta.warmup': 'Warming up, then entering algorithm learning',
        'sgp41.meta.learningRemaining': 'Algorithm learning in progress, about {remaining} remaining',
        'sgp41.meta.waitingSamples': 'Waiting for stable samples',
        'pressureTrend.offline': 'BMP390 offline',
        'pressureTrend.waitingSample': 'Waiting for a valid pressure sample',
        'pressureTrend.waitingHistory': 'About 1 hour of continuous pressure history is required',
        'pressureTrend.meta': '{span} minutes normalized to a 3-hour pressure change of {delta} hPa',
        'pressureTrend.summary': '{label}, based on the last {span} minutes',
        'rainOutlook.season': 'Hangzhou {season}',
        'rainOutlook.seasonNeutral': 'Time is not synced, so seasonality is treated as neutral',
        'rainOutlook.pressure': 'Pressure {delta} hPa/3h',
        'rainOutlook.humidityTrend': 'Humidity {delta} %RH/3h',
        'rainOutlook.currentHumidity': 'Current humidity {value} %RH',
        'rainOutlook.humidityUnavailable': 'Humidity trend unavailable',
        'rainOutlook.dewPointSpread': 'Dew point spread {value} °C',
        'ota.authSavedHint': 'MQTT credentials are stored. For security reasons the page never echoes the username or password back; enter the full MQTT URL again if you need to change them.',
        'ota.noFileSelected': 'No file selected',
        'ota.noGithubAutoUpdate': 'GitHub auto-update is not enabled in this firmware, but manual OTA upload is still available.',
        'ota.meta.currentLatest': 'Current {current}, latest {latest}',
        'ota.meta.latestVersion': 'Latest GitHub version {latest}',
        'ota.meta.asset': 'Update asset {asset}',
        'ota.meta.autoRefresh': 'The page checks the latest GitHub release once on load and refreshes automatically every 2 minutes.',
        'ota.meta.alreadyLatest': 'This device is already on the latest GitHub release',
        'ota.notice.autoNewVersion': 'A new GitHub firmware {version} was detected and the page refreshed automatically. You can update immediately.',
        'ota.checkButtonBusy': 'Checking...',
        'ota.updateButtonBusy': 'Downloading...',
        'ota.updateButtonDirect': 'Update from GitHub',
        'ota.uploadButtonBusy': 'Uploading...',
        'ota.uploadButtonProcessing': 'Processing...',
        'toggle.asc.unavailable': 'SCD41 unavailable',
        'toggle.sps30.unavailable': 'SPS30 unavailable',
        'toggle.rgb.unavailable': 'RGB LED unavailable',
        'overview.mode': 'Mode',
        'overview.wifi': 'Wi-Fi',
        'overview.mqtt': 'MQTT',
        'overview.ip': 'LAN IP',
        'overview.deviceId': 'Device ID',
        'overview.firmware': 'Firmware',
        'overview.co2Compensation': 'CO2 Compensation',
        'overview.uptime': 'Uptime',
        'overview.lastError': 'Last Error',
        'summary.overall': 'Overall Air Quality: {value}',
        'metrics.pmAqi': 'Estimated PM AQI',
        'metrics.co2': 'CO2 Concentration',
        'metrics.indoorTemp': 'Indoor Temperature',
        'metrics.relHumidity': 'Relative Humidity',
        'metrics.pressure': 'Pressure',
        'metrics.pressureTrend': '3-Hour Pressure Trend',
        'metrics.rainChance': 'Short-Term Rain Chance',
        'metrics.vocIndex': 'VOC Index (Sensirion)',
        'metrics.noxIndex': 'NOx Index (Sensirion)',
        'metrics.scd41Unavailable': 'SCD41 unavailable',
        'particle.section.mass': 'PM Mass Concentration',
        'particle.section.count': 'Particle Count',
        'particle.pm1Meta': 'Ultrafine to fine particle mass concentration',
        'particle.pm25Meta': 'Most widely used fine-particle indicator',
        'particle.pm4Meta': 'Intermediate band between PM2.5 and PM10',
        'particle.pm10Meta': 'Total inhalable particulate mass',
        'particle.count05Meta': 'Total particle count below 0.5 µm',
        'particle.count10Meta': 'Total particle count below 1.0 µm',
        'particle.count25Meta': 'Total particle count below 2.5 µm',
        'particle.count40Meta': 'Total particle count below 4.0 µm',
        'particle.count100Meta': 'Total particle count below 10.0 µm',
        'action.configSaveFailed': 'Failed to save configuration',
        'action.configSavedRestartRuntime': 'MQTT configuration was saved. The system will restart to reconnect, and the SCD41 compensation settings were applied immediately.',
        'action.configSavedRestart': 'MQTT configuration was saved. The system will restart to reconnect to the broker.',
        'action.runtimeApplied': 'SCD41 compensation settings were applied immediately.',
        'action.configSaved': 'Configuration saved.',
        'action.operationFailed': 'Operation failed',
        'action.ascEnabled': 'ASC enabled',
        'action.ascDisabled': 'ASC disabled',
        'action.sps30Sleeping': 'SPS30 sleeping',
        'action.sps30Waking': 'SPS30 waking up',
        'action.ledEnabled': 'Status LED enabled',
        'action.ledDisabled': 'Status LED disabled',
        'action.fanCleaningFailed': 'Failed to start fan cleaning',
        'action.fanCleaningStarted': 'SPS30 fan cleaning started. PM data may be briefly unavailable for about 10 seconds.',
        'action.frcFailed': 'FRC calibration failed',
        'action.frcDone': 'Forced calibration applied',
        'action.requestFailed': 'Request failed',
        'action.discoveryRepublished': 'Discovery payloads republished',
        'action.restartRequested': 'Restart requested',
        'action.factoryResetConfirm': '⚠️ This will erase all Wi-Fi and MQTT settings. Continue?',
        'action.factoryResetFailed': 'Failed',
        'action.factoryResetDone': 'The device will finish reinitializing shortly',
        'action.githubCheckFailed': 'Failed to check the latest GitHub release',
        'action.githubNewVersion': 'New version {version} detected and ready to install',
        'action.githubAlreadyLatest': 'This device is already on the latest GitHub release',
        'action.githubUpdateConfirm': 'The device will download the {version} OTA firmware from GitHub and reboot automatically. Continue?',
        'action.githubLatestVersionSuffix': ' the latest version',
        'action.githubUpdateStartFailed': 'Failed to start GitHub OTA update',
        'action.githubUpdateStarted': 'GitHub OTA download started. Do not remove power.',
        'action.selectFirmware': 'Choose a firmware file first',
        'action.uploadingFirmware': 'Uploading firmware...',
        'action.uploadComplete': 'Upload complete, validating and switching firmware...',
        'action.uploadFailed': 'Upload failed',
        'action.uploadNetworkInterrupted': 'OTA update failed: network connection was interrupted',
        'action.uploadValidated': 'Firmware validated. The device will reboot shortly',
        'action.uploadSuccess': 'Firmware upload and validation succeeded. Rebooting soon.',
        'action.uploadRetry': 'Update failed, please try again'
      }
    };

    const labelMaps = {
      'zh-CN': {
        category: { 'Good': '良好', 'Moderate': '一般', 'Unhealthy for Sensitive Groups': '敏感人群不健康', 'Unhealthy': '不健康', 'Very Unhealthy': '非常不健康', 'Hazardous': '危险', 'Unavailable': '暂不可用' },
        co2Signal: { 'Well Ventilated': '通风良好', 'Acceptable Ventilation': '通风可接受', 'Stale Air': '空气发闷', 'Poor Ventilation': '通风较差', 'Very Poor Ventilation': '通风很差', 'Unavailable': '暂不可用' },
        vocSignal: { 'Below Baseline': '低于近期基线', 'Near Baseline': '接近近期基线', 'VOC Event': 'VOC 事件', 'Strong VOC Event': '明显 VOC 事件', 'Severe VOC Event': '强 VOC 事件', 'Unavailable': '暂不可用' },
        noxSignal: { 'Background': '背景水平', 'Trace NOx Event': '轻微 NOx 事件', 'NOx Event': 'NOx 事件', 'High NOx Event': '明显 NOx 事件', 'Severe NOx Event': '强 NOx 事件', 'Unavailable': '暂不可用' },
        comfort: { 'Low': '偏低', 'Acceptable': '可接受', 'High': '偏高', 'Unavailable': '暂不可用' },
        profile: { 'Fine-Mode Dominant': '细颗粒模态主导', 'Mixed-Mode': '细粗混合', 'Coarse-Mode Dominant': '粗颗粒模态主导', 'Unavailable': '暂不可用' },
        particleSituation: { 'Clean Background': '背景水平较低', 'Fine Particle Source Event': '细颗粒源事件', 'Fine Particle Build-up': '细颗粒累积', 'Dust / Coarse Disturbance': '扬尘/粗颗粒扰动', 'Mixed Activity Episode': '混合活动场景', 'Unavailable': '暂不可用' },
        pressureTrend: { 'Rising Fast': '快速回升', 'Rising': '缓慢回升', 'Stable': '基本稳定', 'Falling': '持续下降', 'Falling Fast': '快速下降', 'Unavailable': '暂不可用' },
        rainOutlook: { 'Unlikely': '暂不明显', 'Slight Chance': '略有升高', 'Possible': '可能有雨', 'Likely Soon': '较可能短时降雨', 'Unavailable': '暂不可用' },
        otaState: { idle: '等待检查', checking: '正在查询', uploading: '正在上传', downloading: '正在下载', ready: '可直接升级', success: '升级完成', error: '升级失败' },
        rainSeason: { 'Spring': '春季', 'Meiyu Season': '梅雨季', 'Summer': '夏季', 'Autumn': '秋季', 'Winter': '冬季', 'Unavailable': '未校时' },
        basis: { 'EPA AQI (PM2.5 / PM10)': 'EPA AQI（PM2.5 / PM10）', 'EPA AQI unavailable': 'EPA AQI 暂不可用', 'U.S. indoor humidity guidance': '美国室内湿度参考', 'U.S. indoor ventilation proxy': '美国室内通风参考', 'U.S. indoor ventilation/humidity guidance': '美国室内通风/湿度参考', 'EPA AQI with U.S. indoor guidance': 'EPA AQI + 室内参考', 'No sensor data': '暂无传感器数据', 'Unavailable': '未提供' }
      },
      en: {
        category: { 'Good': 'Good', 'Moderate': 'Moderate', 'Unhealthy for Sensitive Groups': 'Unhealthy for Sensitive Groups', 'Unhealthy': 'Unhealthy', 'Very Unhealthy': 'Very Unhealthy', 'Hazardous': 'Hazardous', 'Unavailable': 'Unavailable' },
        co2Signal: { 'Well Ventilated': 'Well Ventilated', 'Acceptable Ventilation': 'Acceptable Ventilation', 'Stale Air': 'Stale Air', 'Poor Ventilation': 'Poor Ventilation', 'Very Poor Ventilation': 'Very Poor Ventilation', 'Unavailable': 'Unavailable' },
        vocSignal: { 'Below Baseline': 'Below Baseline', 'Near Baseline': 'Near Baseline', 'VOC Event': 'VOC Event', 'Strong VOC Event': 'Strong VOC Event', 'Severe VOC Event': 'Severe VOC Event', 'Unavailable': 'Unavailable' },
        noxSignal: { 'Background': 'Background', 'Trace NOx Event': 'Trace NOx Event', 'NOx Event': 'NOx Event', 'High NOx Event': 'High NOx Event', 'Severe NOx Event': 'Severe NOx Event', 'Unavailable': 'Unavailable' },
        comfort: { 'Low': 'Low', 'Acceptable': 'Acceptable', 'High': 'High', 'Unavailable': 'Unavailable' },
        profile: { 'Fine-Mode Dominant': 'Fine-Mode Dominant', 'Mixed-Mode': 'Mixed-Mode', 'Coarse-Mode Dominant': 'Coarse-Mode Dominant', 'Unavailable': 'Unavailable' },
        particleSituation: { 'Clean Background': 'Clean Background', 'Fine Particle Source Event': 'Fine Particle Source Event', 'Fine Particle Build-up': 'Fine Particle Build-up', 'Dust / Coarse Disturbance': 'Dust / Coarse Disturbance', 'Mixed Activity Episode': 'Mixed Activity Episode', 'Unavailable': 'Unavailable' },
        pressureTrend: { 'Rising Fast': 'Rising Fast', 'Rising': 'Rising', 'Stable': 'Stable', 'Falling': 'Falling', 'Falling Fast': 'Falling Fast', 'Unavailable': 'Unavailable' },
        rainOutlook: { 'Unlikely': 'Unlikely', 'Slight Chance': 'Slight Chance', 'Possible': 'Possible', 'Likely Soon': 'Likely Soon', 'Unavailable': 'Unavailable' },
        otaState: { idle: 'Waiting to Check', checking: 'Checking', uploading: 'Uploading', downloading: 'Downloading', ready: 'Ready to Update', success: 'Update Complete', error: 'Update Failed' },
        rainSeason: { 'Spring': 'Spring', 'Meiyu Season': 'Meiyu Season', 'Summer': 'Summer', 'Autumn': 'Autumn', 'Winter': 'Winter', 'Unavailable': 'Time Unsynced' },
        basis: { 'EPA AQI (PM2.5 / PM10)': 'EPA AQI (PM2.5 / PM10)', 'EPA AQI unavailable': 'EPA AQI unavailable', 'U.S. indoor humidity guidance': 'U.S. indoor humidity guidance', 'U.S. indoor ventilation proxy': 'U.S. indoor ventilation proxy', 'U.S. indoor ventilation/humidity guidance': 'U.S. indoor ventilation/humidity guidance', 'EPA AQI with U.S. indoor guidance': 'EPA AQI with U.S. indoor guidance', 'No sensor data': 'No sensor data', 'Unavailable': 'Unavailable' }
      }
    };

    const categoryLabels = 'category';
    const co2SignalLabels = 'co2Signal';
    const vocSignalLabels = 'vocSignal';
    const noxSignalLabels = 'noxSignal';
    const comfortLabels = 'comfort';
    const profileLabels = 'profile';
    const particleSituationLabels = 'particleSituation';
    const pressureTrendLabels = 'pressureTrend';
    const rainOutlookLabels = 'rainOutlook';
    const otaStateLabels = 'otaState';
    const rainSeasonLabels = 'rainSeason';
    const basisLabels = 'basis';

    function detectInitialLanguage() {
      try {
        const saved = localStorage.getItem(languageStorageKey);
        if (supportedLanguages.includes(saved)) return saved;
      } catch (_) { }
      const browserLanguage = (navigator.language || '').toLowerCase();
      return browserLanguage.startsWith('zh') ? 'zh-CN' : 'en';
    }

    let currentLanguage = detectInitialLanguage();

    function t(key, params = {}) {
      const localized = translations[currentLanguage]?.[key];
      const fallback = translations['zh-CN'][key];
      const template = localized != null ? localized : (fallback != null ? fallback : key);
      return String(template).replace(/\{(\w+)\}/g, (_, name) => (params[name] != null ? params[name] : ''));
    }

    function translate(mapName, value, fallback = t('common.notProvided')) {
      const map = labelMaps[currentLanguage]?.[mapName] || labelMaps['zh-CN']?.[mapName] || {};
      return map[value] || value || fallback;
    }

    function joinLocalized(parts) {
      return parts.join(t('common.listSeparator'));
    }

    function applyStaticTranslations() {
      document.title = t('pageTitle');
      document.documentElement.lang = currentLanguage;
      document.querySelectorAll('[data-i18n]').forEach((el) => {
        el.textContent = t(el.dataset.i18n);
      });
      document.querySelectorAll('[data-i18n-html]').forEach((el) => {
        el.innerHTML = t(el.dataset.i18nHtml);
      });
      document.querySelectorAll('[data-i18n-placeholder]').forEach((el) => {
        el.placeholder = t(el.dataset.i18nPlaceholder);
      });
      document.querySelectorAll('[data-i18n-aria-label]').forEach((el) => {
        el.setAttribute('aria-label', t(el.dataset.i18nAriaLabel));
      });
      languageButtons.forEach((btn) => {
        const active = btn.dataset.lang === currentLanguage;
        btn.setAttribute('aria-pressed', String(active));
      });
    }

    function setLanguage(nextLanguage) {
      if (!supportedLanguages.includes(nextLanguage) || nextLanguage === currentLanguage) return;
      currentLanguage = nextLanguage;
      try { localStorage.setItem(languageStorageKey, currentLanguage); } catch (_) { }
      applyStaticTranslations();
      syncFirmwareLabel();
      if (latestStatusData) {
        renderView(latestStatusData);
        syncActionAvailability(latestStatusData);
        setConfigValues(latestStatusData);
      }
    }

    applyStaticTranslations();
    function formatTempOffsetValue(value) {
      if (value == null || value === '') return '';
      const n = Number(value);
      return Number.isNaN(n) ? '' : n.toFixed(1);
    }
    function roundToSingleDecimal(value) {
      const n = Number(value);
      return Number.isNaN(n) ? 0 : Math.round(n * 10) / 10;
    }
    function formatSigned(value, digits = 1) {
      const n = Number(value);
      if (Number.isNaN(n)) return '--';
      const abs = Math.abs(n).toFixed(digits);
      if (n > 0) return `+${abs}`;
      if (n < 0) return `-${abs}`;
      return Number(abs).toFixed(digits);
    }
    function num(value, digits = 0, unit = '') {
      if (value == null || value === '') return '--';
      const n = Number(value);
      if (Number.isNaN(n)) return '--';
      const v = digits === null ? n : n.toFixed(digits);
      return unit ? `${v}<em class="kv-unit">${unit}</em>` : `${v}`;
    }
    function toFiniteNumber(value) {
      if (value == null || value === '') return Number.NaN;
      const n = Number(value);
      return Number.isFinite(n) ? n : Number.NaN;
    }
    function fmtAge(seconds) {
      if (seconds == null || seconds === '') return '--';
      const sec = Number(seconds);
      if (Number.isNaN(sec)) return '--';
      if (sec < 60) return `${sec} ${t('time.sec')}`;
      const min = Math.floor(sec / 60);
      const rem = sec % 60;
      if (min < 60) return rem ? `${min} ${t('time.min')} ${rem} ${t('time.sec')}` : `${min} ${t('time.min')}`;
      const hr = Math.floor(min / 60);
      const m = min % 60;
      return m ? `${hr} ${t('time.hour')} ${m} ${t('time.min')}` : `${hr} ${t('time.hour')}`;
    }

    // Components
    function kv(label, value) { return `<div class='kv'><span>${label}</span><strong>${value || '--'}</strong></div>`; }
    function particleCard(label, value, meta = '') {
      return `<article class='particle-card'><span class='particle-card-label'>${label}</span><strong class='particle-card-value'>${value || '--'}</strong>${meta ? `<small class='particle-card-meta'>${meta}</small>` : ''}</article>`;
    }
    function metric(label, value, meta = '', variant = 'secondary', unitBadge = '', tone = '') { return `<div class='metric metric-${variant}${tone ? ` metric-tone-${tone}` : ''}'><span>${label}</span><div class='metric-value'><strong>${value || '--'}</strong>${unitBadge ? `<em class='metric-unit'>${unitBadge}</em>` : ''}</div>${meta ? `<small>${meta}</small>` : ''}</div>`; }
    function summaryBanner(value, detail, tone = 'neutral') { return `<div class='summary-banner summary-${tone || 'neutral'}'><strong>${value}</strong><span>${detail}</span></div>`; }

    function localizedOverall(value) { return translate(categoryLabels, value, t('common.assessmentUnavailable')); }
    function particleSituationKey(d) {
      return d?.snapshot?.particle_situation_key || 'unavailable';
    }
    function particleSupportNotes(d) {
      const parts = [];
      const co2 = toFiniteNumber(d?.snapshot?.co2_ppm);
      const humidity = toFiniteNumber(d?.snapshot?.humidity_rh);
      const temperature = toFiniteNumber(d?.snapshot?.temperature_c);
      const situation = particleSituationKey(d);
      if (Number.isFinite(co2) && co2 > 1000) {
        parts.push(t('particle.support.co2High', { value: Math.round(co2) }));
      } else if (Number.isFinite(co2) && co2 > 0 && (situation === 'fine-source' || situation === 'background')) {
        parts.push(t('particle.support.co2Low', { value: Math.round(co2) }));
      }
      if (Number.isFinite(humidity) && humidity < 35) {
        parts.push(t('particle.support.rhLow', { value: Math.round(humidity) }));
      } else if (Number.isFinite(humidity) && humidity > 60 &&
        (situation === 'fine-source' || situation === 'fine-stale')) {
        parts.push(t('particle.support.rhHighFine', { value: Math.round(humidity) }));
      }
      if (Number.isFinite(temperature) && temperature >= 27 &&
        (situation === 'fine-stale' || situation === 'mixed-activity')) {
        parts.push(t('particle.support.tempWarm', { value: temperature.toFixed(1) }));
      } else if (Number.isFinite(temperature) && temperature <= 18 && situation === 'background') {
        parts.push(t('particle.support.tempCool', { value: temperature.toFixed(1) }));
      }
      return parts;
    }
    function particleSummaryTone(d) {
      switch (particleSituationKey(d)) {
        case 'background':
          return 'good';
        case 'fine-source':
        case 'fine-stale':
        case 'coarse-dust':
        case 'mixed-activity':
          return 'moderate';
        default:
          return 'neutral';
      }
    }
    function particleSection(title, items) {
      return `<section class='particle-section'><h4>${title}</h4><div class='particle-grid particle-section-grid'>${items.join('')}</div></section>`;
    }
    function simpleSituationText(d) {
      switch (particleSituationKey(d)) {
        case 'background': return t('particle.simpleScene.background');
        case 'fine-source': return t('particle.simpleScene.fine-source');
        case 'fine-stale': return t('particle.simpleScene.fine-stale');
        case 'coarse-dust': return t('particle.simpleScene.coarse-dust');
        case 'mixed-activity': return t('particle.simpleScene.mixed-activity');
        default: return t('particle.simpleScene.unavailable');
      }
    }
    function simpleGuidanceText(d) {
      switch (particleSituationKey(d)) {
        case 'background': return t('particle.simpleGuidance.background');
        case 'fine-source': return t('particle.simpleGuidance.fine-source');
        case 'fine-stale': return t('particle.simpleGuidance.fine-stale');
        case 'coarse-dust': return t('particle.simpleGuidance.coarse-dust');
        case 'mixed-activity': return t('particle.simpleGuidance.mixed-activity');
        default: return t('particle.simpleGuidance.unavailable');
      }
    }
    function insightChip(label, value, meta, tone) {
      const cls = tone === 'unavailable' ? ' insight-chip-unavailable' : '';
      return `<div class="insight-chip${cls}"><span class="chip-label">${label}</span><span class="chip-value">${value}</span><span class="chip-meta">${meta}</span></div>`;
    }
    function renderAirInsightCard(d, vocMetric, noxMetric) {
      const tone = particleSummaryTone(d);
      const situationLabel = translate(particleSituationLabels, d.snapshot.particle_situation, t('common.unavailable'));
      const descText = simpleSituationText(d);
      const guidanceText = simpleGuidanceText(d);
      const support = particleSupportNotes(d);
      const supportHtml = support.length
        ? `<p class="insight-desc" style="margin-bottom:6px;opacity:0.82;font-size:0.9rem">${support.join(currentLanguage === 'zh-CN' ? '；' : '; ')}</p>`
        : '';
      const chips = [
        insightChip('VOC', vocMetric.value, vocMetric.meta, vocMetric.tone),
        insightChip('NOx', noxMetric.value, noxMetric.meta, noxMetric.tone)
      ];
      return `<div class="air-insight-unified summary-${tone}">` +
        `<div class="insight-title">${t('particle.insightTitle')} · ${situationLabel}</div>` +
        `<p class="insight-desc">${descText}</p>` +
        `<div class="air-insight-indicators">${chips.join('')}</div>` +
        supportHtml +
        `<div class="air-insight-guidance">💡 ${guidanceText}</div>` +
        `</div>`;
    }

    function wifiStatus(d) {
      if (d.diag.provisioning_mode) return t('wifi.waitingProvisioning');
      if (d.diag.wifi_connected) return t('common.connectedRssi', { rssi: d.diag.wifi_rssi });
      return t('common.offline');
    }
    function mqttStatus(d) {
      if (!d.config.mqtt_url) return t('common.unconfigured');
      return d.diag.mqtt_connected ? t('common.connected') : t('common.disconnected');
    }
    function lastErrorStatus(d) {
      const text = (d.diag.last_error || '').trim();
      if (!text || text === 'none') return t('common.none');
      return text;
    }
    function co2CompensationLabel(d) {
      switch (d.snapshot.co2_compensation_source) {
        case 'bmp390': return t('co2Comp.bmp390');
        case 'altitude': return t('co2Comp.altitude');
        default: return d.diag.scd41_ready ? t('co2Comp.inactive') : t('co2Comp.offline');
      }
    }
    function sgp41ModuleStatus(d) {
      if (!d.diag.sgp41_ready) return t('common.offline');
      if (d.snapshot.sgp41_conditioning) return t('sgp41.status.warmup');
      if (d.snapshot.sgp41_voc_valid && d.snapshot.sgp41_nox_valid) return t('common.online');
      if (d.snapshot.sgp41_voc_valid) return t('sgp41.status.vocReady');
      if (d.snapshot.sgp41_nox_valid) return t('sgp41.status.noxReady');
      return t('sgp41.status.learning');
    }
    function sgp41MetricState(d, indexKey, validKey, remainingKey, ratingKey, labelMap) {
      if (!d.diag.sgp41_ready) {
        return { value: '--', meta: t('sgp41.meta.offline'), unit: '', tone: 'unavailable' };
      }
      if (d.snapshot.sgp41_conditioning) {
        return { value: '--', meta: t('sgp41.meta.warmup'), unit: '', tone: 'unavailable' };
      }
      if (d.snapshot[validKey]) {
        return {
          value: num(d.snapshot[indexKey]),
          meta: translate(labelMap, d.snapshot[ratingKey]),
          unit: 'index',
          tone: ''
        };
      }

      const remaining = Number(d.snapshot[remainingKey] || 0);
      if (remaining > 0) {
        return {
          value: '--',
          meta: t('sgp41.meta.learningRemaining', { remaining: fmtAge(remaining) }),
          unit: '',
          tone: 'unavailable'
        };
      }
      return { value: '--', meta: t('sgp41.meta.waitingSamples'), unit: '', tone: 'unavailable' };
    }
    function pressureTrendMeta(d) {
      if (!d.diag.bmp390_ready) return t('pressureTrend.offline');
      if (!d.snapshot.bmp390_valid) return t('pressureTrend.waitingSample');
      if (!d.snapshot.pressure_trend_valid) return t('pressureTrend.waitingHistory');
      return t('pressureTrend.meta', {
        span: d.snapshot.pressure_trend_span_min,
        delta: formatSigned(d.snapshot.pressure_trend_hpa_3h)
      });
    }
    function pressureTrendMetricState(d) {
      if (!d.snapshot.pressure_trend_valid) {
        return {
          value: translate(pressureTrendLabels, d.snapshot.pressure_trend, t('common.unavailable')),
          meta: pressureTrendMeta(d),
          unit: '',
          tone: 'unavailable'
        };
      }
      return {
        value: num(d.snapshot.pressure_trend_hpa_3h, 1),
        meta: t('pressureTrend.summary', {
          label: translate(pressureTrendLabels, d.snapshot.pressure_trend),
          span: d.snapshot.pressure_trend_span_min
        }),
        unit: 'hPa/3h',
        tone: ''
      };
    }
    function rainOutlookMeta(d) {
      if (!d.diag.bmp390_ready) return t('pressureTrend.offline');
      if (!d.snapshot.bmp390_valid) return t('pressureTrend.waitingSample');
      if (!d.snapshot.pressure_trend_valid) return t('pressureTrend.waitingHistory');

      const parts = [];
      if (d.snapshot.rain_season && d.snapshot.rain_season !== 'Unavailable') {
        parts.push(t('rainOutlook.season', { season: translate(rainSeasonLabels, d.snapshot.rain_season) }));
      } else {
        parts.push(t('rainOutlook.seasonNeutral'));
      }
      parts.push(t('rainOutlook.pressure', { delta: formatSigned(d.snapshot.pressure_trend_hpa_3h) }));

      if (d.snapshot.humidity_trend_valid) {
        parts.push(t('rainOutlook.humidityTrend', { delta: formatSigned(d.snapshot.humidity_trend_rh_3h) }));
      } else if (d.snapshot.scd41_valid && d.snapshot.humidity_rh != null) {
        parts.push(t('rainOutlook.currentHumidity', { value: num(d.snapshot.humidity_rh, 0) }));
      } else {
        parts.push(t('rainOutlook.humidityUnavailable'));
      }

      if (d.snapshot.dew_point_spread_c != null && Number.isFinite(Number(d.snapshot.dew_point_spread_c))) {
        parts.push(t('rainOutlook.dewPointSpread', { value: num(d.snapshot.dew_point_spread_c, 1) }));
      }
      return joinLocalized(parts);
    }
    function rainOutlookMetricState(d) {
      return {
        value: translate(rainOutlookLabels, d.snapshot.rain_outlook, t('common.unavailable')),
        meta: rainOutlookMeta(d),
        unit: '',
        tone: d.snapshot.pressure_trend_valid ? '' : 'unavailable'
      };
    }

    function syncActionAvailability(d) {
      const scd41Ready = !!d.diag.scd41_ready;
      const sps30Ready = !!d.diag.sps30_ready;
      const sps30CanClean = sps30Ready && !d.snapshot.sps30_sleeping;
      const ledReady = !!d.diag.status_led_ready;
      ascToggle.disabled = !scd41Ready; frcApplyBtn.disabled = !scd41Ready;
      sps30Toggle.disabled = !sps30Ready;
      sps30FanCleaningBtn.disabled = !sps30CanClean;
      statusLedToggle.disabled = !ledReady;
      republishBtn.disabled = !d.diag.mqtt_connected;
    }

    async function getErrorMessage(r, fallback) {
      try { const j = await r.json(); return j.message || fallback; }
      catch (_) { try { return await r.text() || fallback; } catch (__) { return fallback; } }
    }
    function showNotice(type, message) {
      const container = document.getElementById('toast-container');
      const toast = document.createElement('div');
      toast.className = `toast toast-${type}`;
      toast.innerHTML = message;
      container.appendChild(toast);

      setTimeout(() => {
        toast.classList.add('hiding');
        setTimeout(() => toast.remove(), 300);
      }, 4500);
    }
    async function apiRequest(url, options, fallback, successMessage) {
      try {
        const r = await fetch(url, options);
        if (!r.ok) { showNotice('error', await getErrorMessage(r, fallback)); return false; }
        if (successMessage) showNotice('success', successMessage);
        return true;
      } catch (e) {
        showNotice('error', fallback + ': ' + e.message);
        return false;
      }
    }

    async function apiRequestJson(url, options, fallback) {
      try {
        const r = await fetch(url, options);
        if (!r.ok) {
          showNotice('error', await getErrorMessage(r, fallback));
          return null;
        }
        try { return await r.json(); }
        catch (_) { return { status: 'ok' }; }
      } catch (e) {
        showNotice('error', fallback + ': ' + e.message);
        return null;
      }
    }

    function setOtaProgressState(containerEl, labelEl, percentEl, fillEl, { visible, label, progress }) {
      containerEl.hidden = !visible;
      labelEl.textContent = label;
      const clamped = Math.max(0, Math.min(100, Number(progress) || 0));
      percentEl.textContent = `${Math.round(clamped)}%`;
      fillEl.style.width = `${clamped}%`;
    }

    function setOtaUploadState({ visible, uploading, busy, label, progress, uploadButtonText }) {
      otaBusy = busy != null ? !!busy : !!uploading;
      setOtaProgressState(otaProgressEl, otaProgressLabelEl, otaProgressPercentEl, otaProgressFillEl, { visible, label, progress });
      firmwareInput.disabled = otaBusy;
      otaUploadBtn.disabled = otaBusy;
      otaUploadBtn.textContent = uploadButtonText || (otaBusy && uploading
        ? t('ota.uploadButtonBusy')
        : otaBusy ? t('ota.uploadButtonProcessing') : t('ota.uploadButton'));
    }

    function setGithubOtaProgressState({ visible, label, progress }) {
      setOtaProgressState(
        githubOtaProgressEl,
        githubOtaProgressLabelEl,
        githubOtaProgressPercentEl,
        githubOtaProgressFillEl,
        { visible, label, progress }
      );
    }

    function getXhrErrorMessage(xhr, fallback) {
      try {
        const payload = xhr.responseType === 'json' ? xhr.response : JSON.parse(xhr.responseText);
        if (payload && payload.message) return payload.message;
      } catch (_) { }
      return xhr.responseText || fallback;
    }

    function setConfigValues(d) {
      if (!configDirty) {
        for (const k of configFields) {
          const el = configInputs[k];
          if (!el) continue;
          if (k === 'scd41_temp_offset_c') {
            el.value = formatTempOffsetValue(d.config[k]);
            continue;
          }
          el.value = d.config[k] == null ? '' : d.config[k];
        }
      }
      mqttPrivacyHintEl.textContent = d.config.mqtt_auth_configured
        ? t('ota.authSavedHint')
        : t('config.mqtt.urlHint');
      if (document.activeElement !== frcInput) {
        frcInput.value = d.frc_reference_ppm;
      }
    }

    function syncFirmwareLabel() {
      const file = firmwareInput.files && firmwareInput.files[0];
      firmwareNameEl.textContent = file ? file.name : t('ota.noFileSelected');
    }

    function otaProgressSource(ota) {
      return ota?.source || 'none';
    }

    function otaGithubProgressVisible(ota) {
      const source = otaProgressSource(ota);
      return ota?.state === 'downloading' ||
        ((ota?.state === 'success' || ota?.state === 'error') && source === 'github');
    }

    function otaUploadProgressVisible(ota) {
      const source = otaProgressSource(ota);
      return ota?.state === 'uploading' ||
        ((ota?.state === 'success' || ota?.state === 'error') && source === 'upload');
    }

    function buildOtaMetaText(ota) {
      if (!ota || !ota.enabled) {
        return t('ota.noGithubAutoUpdate');
      }
      if (ota.last_error) {
        return ota.last_error;
      }

      const parts = [];
      if (ota.update_available && ota.current_version && ota.latest_version) {
        parts.push(t('ota.meta.currentLatest', { current: ota.current_version, latest: ota.latest_version }));
      } else if (ota.latest_version) {
        parts.push(t('ota.meta.latestVersion', { latest: ota.latest_version }));
      }
      if (ota.asset_name) {
        parts.push(t('ota.meta.asset', { asset: ota.asset_name }));
      }
      if (!parts.length) {
        parts.push(t('ota.meta.autoRefresh'));
      } else if (!ota.update_available && ota.last_checked_at_ms != null) {
        parts.push(t('ota.meta.alreadyLatest'));
      }
      return joinLocalized(parts);
    }

    function syncGithubReleaseTracking(ota, { notifyOnNewVersion = false } = {}) {
      const latestVersion = (ota?.latest_version || '').trim();
      const previousVersion = otaGithubKnownLatestVersion;

      if (latestVersion) {
        otaGithubKnownLatestVersion = latestVersion;
      }

      const shouldNotify = notifyOnNewVersion &&
        latestVersion &&
        ota?.update_available &&
        latestVersion !== previousVersion &&
        latestVersion !== otaGithubNotifiedVersion;

      if (shouldNotify) {
        otaGithubNotifiedVersion = latestVersion;
        showNotice('success', t('ota.notice.autoNewVersion', { version: latestVersion }));
      }
    }

    function renderOtaStatus(ota) {
      const hasOta = !!ota;
      const enabled = !!ota?.enabled;
      githubOtaSectionEl.hidden = !enabled;
      githubLatestVersionTextEl.textContent = ota?.latest_version || '--';
      githubRepoTextEl.textContent = ota?.github_repo || '--';
      githubOtaStatusTextEl.textContent = translate(otaStateLabels, ota?.state, t('ota.idleState'));
      githubOtaMetaEl.textContent = buildOtaMetaText(ota);

      const busy = !!ota?.busy;
      const restartPending = !!ota?.restart_pending;
      const allowCheck = enabled && !busy && !restartPending;
      const allowUpdate = enabled && !busy && !restartPending && !!ota?.update_available;
      githubOtaCheckBtn.disabled = !allowCheck;
      githubOtaUpdateBtn.disabled = !allowUpdate;
      githubOtaCheckBtn.textContent = busy && ota?.state === 'checking' ? t('ota.checkButtonBusy') : t('ota.checkButton');
      githubOtaUpdateBtn.textContent = busy && ota?.state === 'downloading' ? t('ota.updateButtonBusy') : t('ota.updateButtonDirect');

      if (!hasOta) {
        setGithubOtaProgressState({ visible: false, label: t('ota.idleState'), progress: 0 });
        setOtaUploadState({ visible: false, busy: false, label: t('ota.waitingUpload'), progress: 0 });
        return;
      }

      const progressLabel = translate(otaStateLabels, ota.state, t('ota.waitingUpload'));
      setGithubOtaProgressState({
        visible: otaGithubProgressVisible(ota),
        label: progressLabel,
        progress: ota.progress_percent
      });
      setOtaUploadState({
        visible: otaUploadProgressVisible(ota),
        busy: busy || restartPending,
        uploading: ota.state === 'uploading',
        label: progressLabel,
        progress: ota.progress_percent,
        uploadButtonText: ota.state === 'uploading' && (busy || restartPending)
          ? t('ota.uploadButtonBusy')
          : (busy || restartPending) ? t('ota.uploadButtonProcessing') : t('ota.uploadButton')
      });
    }

    function maybeAutoCheckGithubRelease(ota) {
      if (!ota?.enabled || ota?.busy || ota?.restart_pending || otaGithubCheckInFlight) return;
      if (document.visibilityState === 'hidden') return;

      const hasCheckedBefore = ota.last_checked_at_ms != null && Number(ota.last_checked_at_ms) > 0;
      const now = Date.now();

      if (!hasCheckedBefore) {
        if (otaGithubLastAutoCheckAt !== 0 && now - otaGithubLastAutoCheckAt < githubReleaseAutoRefreshMs) {
          return;
        }
        checkGithubRelease({ silent: true, notifyOnNewVersion: true });
        return;
      }

      if (otaGithubLastAutoCheckAt === 0) {
        otaGithubLastAutoCheckAt = now;
        return;
      }

      if (now - otaGithubLastAutoCheckAt < githubReleaseAutoRefreshMs) {
        return;
      }

      checkGithubRelease({ silent: true, notifyOnNewVersion: true });
    }

    function syncAscState(enabled, ready) {
      const active = !!enabled;
      ascToggle.checked = active;
      ascToggle.disabled = !ready;
      ascStatusText.textContent = t('status.currentLabel', {
        value: !ready ? t('toggle.asc.unavailable') : active ? t('common.enabled') : t('common.disabled')
      });
    }

    function syncSps30State(sleeping, ready) {
      const active = !sleeping;
      sps30Toggle.checked = active;
      sps30Toggle.disabled = !ready;
      sps30StatusText.textContent = t('status.currentLabel', {
        value: !ready ? t('toggle.sps30.unavailable') : active ? t('common.running') : t('common.sleeping')
      });
    }

    function syncStatusLedState(enabled, ready) {
      const active = !!enabled;
      statusLedToggle.checked = active;
      statusLedToggle.disabled = !ready;
      statusLedText.textContent = t('status.currentLabel', {
        value: !ready ? t('toggle.rgb.unavailable') : active ? t('common.enabled') : t('common.disabled')
      });
    }

    function shouldShowOverallSummary(d) {
      const overall = d.snapshot.overall_air_quality;
      if (!overall) return false;
      if (d.snapshot.us_aqi == null || !d.snapshot.us_aqi_level) return true;

      const sameCategory = overall === d.snapshot.us_aqi_level;
      const indoorOverride = d.snapshot.overall_air_quality_driver === 'CO2' || d.snapshot.overall_air_quality_driver === 'Humidity';
      return !sameCategory || indoorOverride;
    }

    function setActiveTab(tabName, { persist = true, updateHash = true } = {}) {
      const hasTab = tabButtons.some((btn) => btn.dataset.tab === tabName);
      const nextTab = hasTab ? tabName : 'realtime';
      tabButtons.forEach((btn) => {
        const active = btn.dataset.tab === nextTab;
        btn.setAttribute('aria-selected', String(active));
        btn.tabIndex = active ? 0 : -1;
      });
      tabPanels.forEach((panel) => {
        const isSelected = panel.dataset.tab === nextTab;
        if (isSelected && panel.hidden) {
          panel.classList.remove('fade-enter');
          void panel.offsetWidth;
          panel.classList.add('fade-enter');
        }
        panel.hidden = !isSelected;
      });
      if (persist) {
        try { localStorage.setItem(activeTabStorageKey, nextTab); } catch (_) { }
      }
      if (updateHash && window.location.hash !== `#${nextTab}`) {
        history.replaceState(null, '', `#${nextTab}`);
      }
    }

    function moveTabFocus(currentIndex, offset) {
      const nextIndex = (currentIndex + offset + tabButtons.length) % tabButtons.length;
      tabButtons[nextIndex].focus();
      setActiveTab(tabButtons[nextIndex].dataset.tab);
    }

    function renderView(d) {
      const overallKey = d.snapshot.overall_air_quality_key || 'neutral';
      const vocMetric = sgp41MetricState(
        d,
        'voc_index',
        'sgp41_voc_valid',
        'sgp41_voc_stabilization_remaining_s',
        'voc_rating',
        vocSignalLabels
      );
      const noxMetric = sgp41MetricState(
        d,
        'nox_index',
        'sgp41_nox_valid',
        'sgp41_nox_stabilization_remaining_s',
        'nox_rating',
        noxSignalLabels
      );
      const pressureTrendMetric = pressureTrendMetricState(d);
      const rainOutlookMetric = rainOutlookMetricState(d);

      overviewNetworkEl.innerHTML = [
        kv(t('overview.mode'), d.diag.provisioning_mode ? t('common.bleProvisioning') : t('common.lanMode')),
        kv(t('overview.wifi'), wifiStatus(d)),
        kv(t('overview.mqtt'), mqttStatus(d)),
        kv(t('overview.ip'), d.diag.ip_addr || '--')
      ].join('');

      overviewModulesEl.innerHTML = [
        kv('SCD41', d.diag.scd41_ready ? t('common.online') : t('common.offline')),
        kv('SGP41', sgp41ModuleStatus(d)),
        kv('BMP390', d.diag.bmp390_ready ? (d.snapshot.bmp390_valid ? t('common.online') : t('common.connectedOnly')) : t('common.offline')),
        kv('SPS30', d.diag.sps30_ready ? (d.snapshot.sps30_sleeping ? t('common.sleeping') : t('common.online')) : t('common.offline')),
        kv(t('maintenance.switches.rgbTitle'), d.diag.status_led_ready ? (d.diag.status_led_enabled ? t('common.enabled') : t('common.disabled')) : t('common.unavailable'))
      ].join('');

      overviewSystemEl.innerHTML = [
        kv(t('overview.deviceId'), d.diag.device_id),
        kv(t('overview.firmware'), d.diag.firmware_version || '--'),
        kv(t('overview.co2Compensation'), co2CompensationLabel(d)),
        kv(t('overview.uptime'), fmtAge(d.diag.uptime_sec)),
        kv(t('overview.lastError'), lastErrorStatus(d))
      ].join('');

      firmwareVersionTextEl.textContent = d.diag.firmware_version || '--';
      renderOtaStatus(d.ota);
      syncGithubReleaseTracking(d.ota);
      maybeAutoCheckGithubRelease(d.ota);

      airSummaryEl.innerHTML = shouldShowOverallSummary(d)
        ? summaryBanner(
          t('summary.overall', { value: localizedOverall(d.snapshot.overall_air_quality) }),
          `${translate(basisLabels, d.snapshot.overall_air_quality_basis)}`,
          overallKey
        )
        : '';

      airMetricsPrimaryEl.innerHTML = [
        metric(t('metrics.pmAqi'), num(d.snapshot.us_aqi), translate(categoryLabels, d.snapshot.us_aqi_level), 'featured', '', d.snapshot.us_aqi_level_key || 'unavailable'),
        metric(t('metrics.co2'), num(d.snapshot.co2_ppm), translate(co2SignalLabels, d.snapshot.co2_rating), 'primary', 'ppm')
      ].join('');

      airMetricsSecondaryEl.innerHTML = [
        metric(
          t('metrics.indoorTemp'),
          num(d.snapshot.temperature_c, 1),
          d.snapshot.scd41_valid
            ? `SCD41${currentLanguage === 'zh-CN' ? '，' : ', '}${translate(comfortLabels, d.snapshot.temperature_rating, '--')}`
            : t('metrics.scd41Unavailable'),
          'secondary',
          '°C'
        ),
        metric(t('metrics.relHumidity'), num(d.snapshot.humidity_rh, 1), translate(comfortLabels, d.snapshot.humidity_rating, '--'), 'secondary', '%'),
        metric(t('metrics.pressure'), num(d.snapshot.pressure_hpa, 1), d.snapshot.bmp390_valid ? 'BMP390' : (d.diag.bmp390_ready ? t('common.connectedOnly') : t('common.offline')), 'secondary', 'hPa'),
        metric(t('metrics.pressureTrend'), pressureTrendMetric.value, pressureTrendMetric.meta, 'secondary', pressureTrendMetric.unit, pressureTrendMetric.tone),
        metric(t('metrics.rainChance'), rainOutlookMetric.value, rainOutlookMetric.meta, 'secondary', rainOutlookMetric.unit, rainOutlookMetric.tone),
        metric(t('metrics.vocIndex'), vocMetric.value, vocMetric.meta, 'secondary', vocMetric.unit, vocMetric.tone),
        metric(t('metrics.noxIndex'), noxMetric.value, noxMetric.meta, 'secondary', noxMetric.unit, noxMetric.tone)
      ].join('');

      syncAscState(d.config.scd41_asc_enabled, d.diag.scd41_ready);
      syncSps30State(d.snapshot.sps30_sleeping, d.diag.sps30_ready);
      syncStatusLedState(d.diag.status_led_enabled, d.diag.status_led_ready);

      particleMetricsEl.innerHTML = `<div class='particle-sections'>${[
        particleSection(t('particle.section.mass'), [
          particleCard('PM1.0', num(d.snapshot.pm1_0, 1, 'µg/m³'), t('particle.pm1Meta')),
          particleCard('PM2.5', num(d.snapshot.pm2_5, 1, 'µg/m³'), t('particle.pm25Meta')),
          particleCard('PM4.0', num(d.snapshot.pm4_0, 1, 'µg/m³'), t('particle.pm4Meta')),
          particleCard('PM10', num(d.snapshot.pm10_0, 1, 'µg/m³'), t('particle.pm10Meta'))
        ]),
        particleSection(t('particle.section.count'), [
          particleCard('<0.5 µm', num(d.snapshot.particles_0_5um, 1, '#/cm³'), t('particle.count05Meta')),
          particleCard('<1.0 µm', num(d.snapshot.particles_1_0um, 1, '#/cm³'), t('particle.count10Meta')),
          particleCard('<2.5 µm', num(d.snapshot.particles_2_5um, 1, '#/cm³'), t('particle.count25Meta')),
          particleCard('<4.0 µm', num(d.snapshot.particles_4_0um, 1, '#/cm³'), t('particle.count40Meta')),
          particleCard('<10.0 µm', num(d.snapshot.particles_10_0um, 1, '#/cm³'), t('particle.count100Meta'))
        ])
      ].join('')}</div>`;

      particleSummaryEl.innerHTML = renderAirInsightCard(d, vocMetric, noxMetric);
    }

    async function fetchStatus() {
      try {
        const r = await fetch('/api/status');
        if (!r.ok) return;
        const d = await r.json();
        latestStatusData = d;
        renderView(d);
        syncActionAvailability(d);
        setConfigValues(d);
      } catch (e) { }
    }

    async function saveConfig() {
      const payload = {
        scd41_altitude_m: Number(configInputs.scd41_altitude_m.value),
        scd41_temp_offset_c: roundToSingleDecimal(configInputs.scd41_temp_offset_c.value)
      };
      if (mqttUrlDirty) {
        payload.mqtt_url = configInputs.mqtt_url.value;
      }
      const result = await apiRequestJson('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) }, t('action.configSaveFailed'));
      if (!result) return;
      configDirty = false;
      mqttUrlDirty = false;
      if (result.restart && result.runtime_applied) {
        showNotice('success', t('action.configSavedRestartRuntime'));
        setTimeout(fetchStatus, 1500);
        return;
      }
      if (result.restart) {
        showNotice('success', t('action.configSavedRestart'));
        setTimeout(fetchStatus, 1500);
        return;
      }
      if (result.runtime_applied) {
        showNotice('success', t('action.runtimeApplied'));
        fetchStatus();
        return;
      }
      showNotice('success', t('action.configSaved'));
      fetchStatus();
    }

    // Actions
    async function toggleAsc(enabled) {
      const ok = await apiRequest('/api/action/scd41-asc', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ enabled }) }, t('action.operationFailed'), enabled ? t('action.ascEnabled') : t('action.ascDisabled'));
      await fetchStatus();
      return ok;
    }
    async function toggleSps30(sleep) {
      const ok = await apiRequest('/api/action/sps30-sleep', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ sleep }) }, t('action.operationFailed'), sleep ? t('action.sps30Sleeping') : t('action.sps30Waking'));
      await fetchStatus();
      return ok;
    }
    async function toggleStatusLed(enabled) {
      const ok = await apiRequest('/api/action/status-led', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ enabled }) }, t('action.operationFailed'), enabled ? t('action.ledEnabled') : t('action.ledDisabled'));
      await fetchStatus();
      return ok;
    }
    async function startSps30FanCleaning() {
      sps30FanCleaningBtn.disabled = true;
      try {
        await apiRequest('/api/action/sps30-fan-cleaning', { method: 'POST' }, t('action.fanCleaningFailed'), t('action.fanCleaningStarted'));
      } finally {
        await fetchStatus();
      }
    }
    async function applyFrc() { await apiRequest('/api/action/apply-frc', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ppm: Number(frcInput.value) }) }, t('action.frcFailed'), t('action.frcDone')); fetchStatus(); }
    async function republishDiscovery() { await apiRequest('/api/action/republish-discovery', { method: 'POST' }, t('action.requestFailed'), t('action.discoveryRepublished')); }
    async function restartDevice() { await apiRequest('/api/action/restart', { method: 'POST' }, t('action.requestFailed'), t('action.restartRequested')); }
    async function factoryReset() { if (confirm(t('action.factoryResetConfirm'))) { await apiRequest('/api/action/factory-reset', { method: 'POST' }, t('action.factoryResetFailed'), t('action.factoryResetDone')); } }
    async function checkGithubRelease({ silent = false, notifyOnNewVersion = false } = {}) {
      if (otaBusy || otaGithubCheckInFlight) return false;
      otaGithubCheckInFlight = true;
      otaGithubLastAutoCheckAt = Date.now();
      try {
        const r = await fetch('/api/ota/github/check', { method: 'POST' });
        if (!r.ok) {
          if (!silent) {
            showNotice('error', await getErrorMessage(r, t('action.githubCheckFailed')));
          }
          return false;
        }

        const payload = await r.json();
        if (payload?.ota) {
          renderOtaStatus(payload.ota);
          syncGithubReleaseTracking(payload.ota, { notifyOnNewVersion });
        }

        if (!silent) {
          if (payload?.ota?.update_available) {
            showNotice('success', t('action.githubNewVersion', { version: payload.ota.latest_version }));
          } else {
            showNotice('info', t('action.githubAlreadyLatest'));
          }
        }
        return true;
      } catch (e) {
        if (!silent) {
          showNotice('error', `${t('action.githubCheckFailed')}: ${e.message}`);
        }
        return false;
      } finally {
        otaGithubCheckInFlight = false;
      }
    }
    async function updateFirmwareFromGithub() {
      if (otaBusy) return;
      const latestVersion = githubLatestVersionTextEl.textContent && githubLatestVersionTextEl.textContent !== '--'
        ? ` v${githubLatestVersionTextEl.textContent}`
        : t('action.githubLatestVersionSuffix');
      if (!confirm(t('action.githubUpdateConfirm', { version: latestVersion }))) {
        return;
      }

      const result = await apiRequestJson('/api/ota/github/update', { method: 'POST' }, t('action.githubUpdateStartFailed'));
      if (!result) return;
      if (result.ota) {
        renderOtaStatus(result.ota);
      }
      showNotice('success', t('action.githubUpdateStarted'));
    }
    async function uploadFirmware() {
      if (otaBusy) return;
      const file = firmwareInput.files[0];
      if (!file) { showNotice('info', t('action.selectFirmware')); return; }

      setOtaUploadState({ visible: true, uploading: true, busy: true, label: t('action.uploadingFirmware'), progress: 0, uploadButtonText: t('ota.uploadButtonBusy') });

      await new Promise((resolve) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/ota');
        xhr.responseType = 'json';

        xhr.upload.onprogress = (event) => {
          if (!event.lengthComputable) {
            setOtaUploadState({ visible: true, uploading: true, busy: true, label: t('action.uploadingFirmware'), progress: 0, uploadButtonText: t('ota.uploadButtonBusy') });
            return;
          }
          const percent = event.total > 0 ? (event.loaded / event.total) * 100 : 0;
          setOtaUploadState({ visible: true, uploading: true, busy: true, label: t('action.uploadingFirmware'), progress: percent, uploadButtonText: t('ota.uploadButtonBusy') });
        };

        xhr.upload.onload = () => {
          setOtaUploadState({ visible: true, uploading: true, busy: true, label: t('action.uploadComplete'), progress: 100, uploadButtonText: t('ota.uploadButtonBusy') });
        };

        xhr.onerror = () => {
          setOtaUploadState({ visible: true, uploading: false, busy: false, label: t('action.uploadFailed'), progress: 0, uploadButtonText: t('ota.uploadButton') });
          showNotice('error', t('action.uploadNetworkInterrupted'));
          resolve();
        };

        xhr.onload = () => {
          if (xhr.status >= 200 && xhr.status < 300) {
            setOtaUploadState({ visible: true, uploading: false, busy: true, label: t('action.uploadValidated'), progress: 100, uploadButtonText: t('ota.uploadButtonProcessing') });
            showNotice('success', t('action.uploadSuccess'));
            resolve();
            return;
          }

          setOtaUploadState({ visible: true, uploading: false, busy: false, label: t('action.uploadRetry'), progress: 0, uploadButtonText: t('ota.uploadButton') });
          showNotice('error', getXhrErrorMessage(xhr, t('action.uploadRetry')));
          resolve();
        };

        xhr.send(file);
      });
    }

    for (const key of configFields) {
      const el = document.getElementById(key);
      if (el) {
        el.addEventListener('input', () => {
          configDirty = true;
          if (key === 'mqtt_url') mqttUrlDirty = true;
        });
      }
    }
    firmwareInput.addEventListener('change', syncFirmwareLabel);
    configInputs.scd41_temp_offset_c.addEventListener('blur', () => {
      const value = formatTempOffsetValue(configInputs.scd41_temp_offset_c.value);
      configInputs.scd41_temp_offset_c.value = value;
    });
    ascToggle.addEventListener('change', async () => {
      const nextValue = ascToggle.checked;
      const previousValue = !nextValue;
      ascToggle.disabled = true;
      const ok = await toggleAsc(nextValue);
      if (!ok) {
        syncAscState(previousValue, true);
      }
    });
    sps30Toggle.addEventListener('change', async () => {
      const nextValue = sps30Toggle.checked;
      const previousValue = !nextValue;
      sps30Toggle.disabled = true;
      const ok = await toggleSps30(!nextValue);
      if (!ok) {
        syncSps30State(!previousValue, true);
      }
    });
    statusLedToggle.addEventListener('change', async () => {
      const nextValue = statusLedToggle.checked;
      const previousValue = !nextValue;
      statusLedToggle.disabled = true;
      const ok = await toggleStatusLed(nextValue);
      if (!ok) {
        syncStatusLedState(previousValue, true);
      }
    });
    languageButtons.forEach((btn) => {
      btn.addEventListener('click', () => setLanguage(btn.dataset.lang));
    });
    tabButtons.forEach((btn, index) => {
      btn.addEventListener('click', () => setActiveTab(btn.dataset.tab));
      btn.addEventListener('keydown', (event) => {
        if (event.key === 'ArrowRight') { event.preventDefault(); moveTabFocus(index, 1); }
        if (event.key === 'ArrowLeft') { event.preventDefault(); moveTabFocus(index, -1); }
        if (event.key === 'Home') { event.preventDefault(); tabButtons[0].focus(); setActiveTab(tabButtons[0].dataset.tab); }
        if (event.key === 'End') { event.preventDefault(); tabButtons[tabButtons.length - 1].focus(); setActiveTab(tabButtons[tabButtons.length - 1].dataset.tab); }
      });
    });
    const initialTab = (() => {
      const hashTab = window.location.hash.replace('#', '');
      if (hashTab) return hashTab;
      try { return localStorage.getItem(activeTabStorageKey) || 'realtime'; }
      catch (_) { return 'realtime'; }
    })();
    syncFirmwareLabel();
    setActiveTab(initialTab, { persist: false, updateHash: !!window.location.hash });
    fetchStatus();
    setInterval(fetchStatus, 5000);
