#include "provisioning_web.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_quality.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char *TAG = "provisioning_web";

#define MQTT_PORT_MIN 1
#define MQTT_PORT_MAX 65535
#define PUBLISH_INTERVAL_MIN 5
#define PUBLISH_INTERVAL_MAX 60
#define SCD41_ALTITUDE_MIN 0
#define SCD41_ALTITUDE_MAX 3000
#define SCD41_TEMP_OFFSET_MIN 0.0
#define SCD41_TEMP_OFFSET_MAX 20.0

typedef struct {
    httpd_handle_t server;
    provisioning_web_callbacks_t callbacks;
    void *user_ctx;
    char device_id[DEVICE_ID_LEN];
} web_ctx_t;

static web_ctx_t s_ctx;

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>空气监测节点后台</title>"
    "<style>"
    ":root{--bg:#f2eadf;--bg-deep:#eadbca;--panel:rgba(255,249,241,.92);--panel-strong:#fff5ea;--ink:#241915;--muted:#6b6058;--accent:#c55a2d;--accent-deep:#8f3d1a;--accent-soft:#f6dcc8;--line:#e4d4c2;--shadow:0 24px 56px rgba(72,47,31,.10);--ok:#26623d;--warn:#8a6415;--danger:#8b2f2f;}"
    "*{box-sizing:border-box;} body{margin:0;font-family:'IBM Plex Sans','Noto Sans SC','PingFang SC','Helvetica Neue',sans-serif;background:linear-gradient(180deg,#f8f2ea 0%,#f0e5d7 100%);color:var(--ink);}"
    "body::before{content:'';position:fixed;inset:0;pointer-events:none;background:radial-gradient(circle at top left,rgba(255,255,255,.75),transparent 28%),radial-gradient(circle at 85% 12%,rgba(197,90,45,.14),transparent 20%),radial-gradient(circle at 20% 80%,rgba(143,61,26,.10),transparent 24%);}"
    ".shell{position:relative;max-width:1240px;margin:0 auto;padding:28px 20px 40px;}"
    ".hero{display:grid;grid-template-columns:minmax(0,1.45fr) minmax(280px,.85fr);gap:18px;margin-bottom:18px;}"
    ".hero-main,.hero-side,.card{background:var(--panel);border:1px solid var(--line);border-radius:28px;box-shadow:var(--shadow);backdrop-filter:blur(10px);}"
    ".hero-main{padding:26px 28px;background:linear-gradient(145deg,rgba(255,248,238,.98),rgba(252,237,223,.92));}"
    ".hero-side{padding:22px 24px;display:flex;flex-direction:column;justify-content:center;}"
    ".eyebrow{display:inline-flex;align-items:center;gap:8px;padding:6px 12px;border-radius:999px;background:var(--accent-soft);color:var(--accent-deep);font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;}"
    ".hero h1{margin:14px 0 10px;font-size:40px;line-height:1.05;letter-spacing:.02em;}"
    ".lede{margin:0;color:var(--muted);max-width:56ch;line-height:1.6;}"
    ".chip-row{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px;}"
    ".chip{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;font-size:12px;font-weight:700;letter-spacing:.04em;background:#efe3d5;color:#734f34;}"
    ".chip-good{background:#d8f2dc;color:#20502e;}.chip-moderate,.chip-acceptable{background:#fff1be;color:#73540f;}.chip-sensitive,.chip-elevated{background:#ffd8b0;color:#7c4611;}.chip-unhealthy,.chip-high{background:#f7c3c3;color:#7b2929;}.chip-very-unhealthy,.chip-very-high,.chip-hazardous{background:#ead8f7;color:#59336b;}.chip-unavailable,.chip-neutral,.chip-mixed{background:#ece3d7;color:#6f6358;}.chip-fine{background:#dcecf8;color:#30506d;}.chip-coarse{background:#f6d8c7;color:#7f4a1d;}"
    ".section-tag{font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin-bottom:12px;}"
    ".grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:18px;align-items:start;}"
    ".span-2{grid-column:span 2;}"
    ".card{padding:22px 22px 20px;}"
    ".card h2{margin:0 0 8px;font-size:19px;letter-spacing:.01em;}"
    ".section-copy{margin:0 0 16px;color:var(--muted);line-height:1.5;font-size:14px;}"
    ".kv-list{display:grid;gap:2px;}.kv-list.compact .kv{padding:9px 0;}"
    ".kv{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px dashed var(--line);align-items:flex-start;}"
    ".kv:last-child{border-bottom:0;}.kv span{color:var(--muted);font-size:14px;line-height:1.45;}.kv strong{font-size:14px;line-height:1.45;text-align:right;max-width:60%;}"
    ".metric-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(132px,1fr));gap:12px;}"
    ".metric{padding:14px 14px 12px;border:1px solid var(--line);border-radius:20px;background:rgba(255,255,255,.55);min-height:96px;display:flex;flex-direction:column;justify-content:space-between;}"
    ".metric span{font-size:12px;color:var(--muted);letter-spacing:.04em;text-transform:uppercase;}"
    ".metric strong{font-size:22px;line-height:1.15;}"
    ".metric small{font-size:12px;color:var(--muted);line-height:1.45;}"
    ".summary-banner{padding:14px 16px;border-radius:22px;margin-bottom:14px;border:1px solid transparent;}"
    ".summary-banner strong{display:block;font-size:22px;line-height:1.15;margin-bottom:6px;}"
    ".summary-banner span{display:block;color:inherit;opacity:.9;font-size:14px;line-height:1.5;}"
    ".summary-good{background:#ddf6e1;color:#184b2a;border-color:#c8eacb;}.summary-moderate,.summary-acceptable{background:#fff4c8;color:#6a4d07;border-color:#f0de97;}.summary-sensitive,.summary-elevated{background:#ffe0c2;color:#7a4312;border-color:#f0c493;}.summary-unhealthy,.summary-high{background:#f8d0d0;color:#7b2c2c;border-color:#e8b0b0;}.summary-very-unhealthy,.summary-very-high,.summary-hazardous{background:#eadbf8;color:#5a3470;border-color:#d9c4ef;}.summary-unavailable,.summary-neutral,.summary-mixed{background:#efe5d9;color:#6d6158;border-color:#e3d6c7;}.summary-fine{background:#dcecf8;color:#2d5574;border-color:#bfd8ea;}.summary-coarse{background:#f6dccd;color:#7e461d;border-color:#ebbea3;}"
    ".field-grid,.action-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;}"
    ".field-span-2{grid-column:span 2;}"
    ".subsection{padding-top:14px;margin-top:14px;border-top:1px dashed var(--line);}"
    ".subsection:first-of-type{padding-top:0;margin-top:0;border-top:0;}"
    ".subsection h3{margin:0 0 8px;font-size:15px;}.subsection p{margin:0 0 12px;color:var(--muted);font-size:13px;line-height:1.5;}"
    "label{display:block;font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:0 0 6px;}"
    ".hint{margin-top:10px;color:var(--muted);font-size:13px;line-height:1.5;}"
    "input,button{font:inherit;border-radius:16px;border:1px solid var(--line);padding:12px 14px;background:#fff;color:var(--ink);width:100%;}"
    "input:focus{outline:2px solid rgba(197,90,45,.20);border-color:var(--accent);} input[type='file']{padding:10px;background:transparent;border:0;}"
    "button{cursor:pointer;background:var(--accent);color:#fff;border:none;font-weight:700;box-shadow:0 12px 24px rgba(197,90,45,.18);transition:transform .18s ease,box-shadow .18s ease,background .18s ease;}"
    "button:hover{transform:translateY(-1px);box-shadow:0 14px 28px rgba(197,90,45,.24);background:var(--accent-deep);}"
    "button:disabled{cursor:not-allowed;opacity:.52;transform:none;box-shadow:none;background:#c9b6a5;color:#fff;}"
    ".ghost{background:transparent;border:1px solid var(--accent);color:var(--accent-deep);box-shadow:none;}.ghost:hover{background:rgba(197,90,45,.08);box-shadow:none;}"
    ".danger{background:#8b2f2f;box-shadow:0 12px 24px rgba(139,47,47,.16);}.danger:hover{background:#6f2222;box-shadow:0 14px 28px rgba(139,47,47,.22);}"
    ".upload{padding:12px 14px;border:1px dashed var(--line);border-radius:20px;background:rgba(255,255,255,.45);}"
    ".notice{margin:0 0 18px;padding:14px 16px;border-radius:18px;font-weight:600;line-height:1.5;}"
    ".notice[hidden]{display:none;}.notice-success{background:#ddf4e3;color:#1f5732;border:1px solid #c5e5ce;}.notice-error{background:#f8d8d8;color:#7e2626;border:1px solid #ebbbbb;}.notice-info{background:#ede4da;color:#66594f;border:1px solid #dfd3c7;}"
    ".footer-note{margin-top:14px;color:var(--muted);font-size:13px;line-height:1.6;}"
    "@media (max-width:1080px){.grid{grid-template-columns:repeat(2,minmax(0,1fr));}.hero{grid-template-columns:1fr;}}"
    "@media (max-width:720px){.shell{padding:18px 14px 30px;}.hero h1{font-size:32px;}.grid,.field-grid,.action-grid{grid-template-columns:1fr;}.span-2,.field-span-2{grid-column:span 1;}.kv{flex-direction:column;}.kv strong{max-width:100%;text-align:left;}.metric strong{font-size:20px;}}"
    "</style></head><body><div class='shell'>"
    "<header class='hero'>"
    "<section class='hero-main'>"
    "<span class='eyebrow'>ESP32-S3 室内空气监测节点</span>"
    "<h1>设备管理后台</h1>"
    "<p class='lede'>统一查看设备状态、空气质量、网络配置、传感器控制和维护操作。界面已按功能重新分区，并统一为中文表达。</p>"
    "<div id='heroBadges' class='chip-row'></div>"
    "</section>"
    "<aside class='hero-side'><div class='section-tag'>当前设备</div><div id='heroMeta' class='kv-list compact'></div></aside>"
    "</header>"
    "<div id='notice' class='notice' hidden></div>"
    "<div class='grid'>"
    "<section class='card span-2'><h2>运行概览</h2><p class='section-copy'>汇总联网、采样和维护状态，便于快速确认后台主要功能是否正常。</p><div id='overview' class='kv-list'></div></section>"
    "<section class='card'><h2>连接与模块</h2><p class='section-copy'>快速确认 Wi-Fi、MQTT、各传感器与指示灯的当前状态。</p><div id='services' class='kv-list'></div></section>"
    "<section class='card span-2'><h2>空气质量总览</h2><p class='section-copy'>统一展示总体评估、美国 AQI 和气体相关补充指标，避免结论分散。</p><div id='airSummary'></div><div id='airMetrics' class='metric-grid'></div></section>"
    "<section class='card'><h2>颗粒物画像</h2><p class='section-copy'>结合 PM 分段、粒子计数与典型粒径，帮助判断颗粒物更偏细颗粒、混合颗粒还是粗颗粒。</p><div id='particleSummary'></div><div id='particleMetrics' class='kv-list'></div></section>"
    "<section class='card'><h2>网络与 MQTT 配置</h2><p class='section-copy'>保存后设备会自动重启并重新应用网络相关配置。</p>"
    "<div class='field-grid'>"
    "<div class='field-span-2'><label>设备名称</label><input id='device_name'/></div>"
    "<div><label>Wi-Fi 名称</label><input id='wifi_ssid'/></div>"
    "<div><label>Wi-Fi 密码</label><input id='wifi_password' type='password'/></div>"
    "<div class='field-span-2'><label>MQTT 主机</label><input id='mqtt_host'/></div>"
    "<div><label>MQTT 端口</label><input id='mqtt_port' type='number'/></div>"
    "<div><label>发布间隔（秒）</label><input id='publish_interval_sec' type='number'/></div>"
    "<div><label>MQTT 用户名</label><input id='mqtt_username'/></div>"
    "<div><label>MQTT 密码</label><input id='mqtt_password' type='password'/></div>"
    "<div><label>Discovery 前缀</label><input id='discovery_prefix'/></div>"
    "<div><label>主题根路径</label><input id='topic_root'/></div>"
    "</div><button onclick='saveConfig()'>保存网络与 MQTT 配置</button></section>"
    "<section class='card'><h2>传感器与指示灯</h2><p class='section-copy'>所有即时控制集中在这里，避免配置项和运行时动作混在一起。</p>"
    "<div class='subsection'><h3>SCD41 校准</h3><p>调整海拔补偿、温度偏移以及自动自校准。</p>"
    "<div class='field-grid'><div><label>海拔补偿（米）</label><input id='scd41_altitude_m' type='number'/></div><div><label>温度偏移（°C）</label><input id='scd41_temp_offset_c' type='number' step='0.1'/></div></div>"
    "<div class='action-grid'><button id='asc_on_btn' onclick='toggleAsc(true)'>开启 ASC</button><button id='asc_off_btn' class='ghost' onclick='toggleAsc(false)'>关闭 ASC</button></div>"
    "</div>"
    "<div class='subsection'><h3>SPS30 与 RGB 指示灯</h3><p>分别控制颗粒物传感器休眠状态和板载 RGB 状态灯。</p>"
    "<div class='action-grid'><button id='sps30_wake_btn' onclick='toggleSps30(false)'>唤醒 SPS30</button><button id='sps30_sleep_btn' class='ghost' onclick='toggleSps30(true)'>让 SPS30 休眠</button><button id='status_led_on_btn' onclick='toggleStatusLed(true)'>打开 RGB 灯</button><button id='status_led_off_btn' class='ghost' onclick='toggleStatusLed(false)'>关闭 RGB 灯</button></div>"
    "</div>"
    "<div class='subsection'><h3>FRC 强制校准</h3><p>仅在满足 SCD41 数据手册前置条件时使用。</p><label>FRC 参考值（ppm）</label><input id='frc_reference_ppm' type='number'/><button id='frc_apply_btn' onclick='applyFrc()'>应用 FRC 强制校准</button></div>"
    "</section>"
    "<section class='card'><h2>系统维护</h2><p class='section-copy'>统一管理 Discovery、OTA 升级和设备重启类操作。</p>"
    "<div class='subsection'><h3>平台联动</h3><p>当 Home Assistant 实体缺失或配置变化后，可重新发布 Discovery。</p><button id='republish_btn' class='ghost' onclick='republishDiscovery()'>重新发布 Discovery</button></div>"
    "<div class='subsection'><h3>固件升级</h3><p>上传新的固件后，设备会在校验通过后自动切换并重启。</p><div class='upload'><input id='firmware' type='file'/><button onclick='uploadFirmware()'>上传并安装固件</button></div></div>"
    "<div class='subsection'><h3>设备操作</h3><p>重启不会清除配置；恢复出厂会清除已保存的配置并重新进入配网流程。</p><div class='action-grid'><button onclick='restartDevice()'>重启设备</button><button class='danger' onclick='factoryReset()'>恢复出厂设置</button></div></div>"
    "<div class='footer-note'>当前管理页面默认面向受信任的局域网环境，不带登录鉴权。</div>"
    "</section>"
    "</div></div><script>"
    "const configFields=['device_name','wifi_ssid','wifi_password','mqtt_host','mqtt_port','mqtt_username','mqtt_password','discovery_prefix','topic_root','publish_interval_sec','scd41_altitude_m','scd41_temp_offset_c'];"
    "const configInputs=Object.fromEntries(configFields.map((id)=>[id,document.getElementById(id)]));"
    "const noticeEl=document.getElementById('notice');"
    "const heroBadgesEl=document.getElementById('heroBadges');"
    "const heroMetaEl=document.getElementById('heroMeta');"
    "const overviewEl=document.getElementById('overview');"
    "const servicesEl=document.getElementById('services');"
    "const airSummaryEl=document.getElementById('airSummary');"
    "const airMetricsEl=document.getElementById('airMetrics');"
    "const particleSummaryEl=document.getElementById('particleSummary');"
    "const particleMetricsEl=document.getElementById('particleMetrics');"
    "const frcInput=document.getElementById('frc_reference_ppm');"
    "const firmwareInput=document.getElementById('firmware');"
    "const ascOnBtn=document.getElementById('asc_on_btn');"
    "const ascOffBtn=document.getElementById('asc_off_btn');"
    "const sps30WakeBtn=document.getElementById('sps30_wake_btn');"
    "const sps30SleepBtn=document.getElementById('sps30_sleep_btn');"
    "const statusLedOnBtn=document.getElementById('status_led_on_btn');"
    "const statusLedOffBtn=document.getElementById('status_led_off_btn');"
    "const frcApplyBtn=document.getElementById('frc_apply_btn');"
    "const republishBtn=document.getElementById('republish_btn');"
    "let configDirty=false;"
    "let noticeTimer=0;"
    "const categoryLabels={'Good':'良好','Moderate':'一般','Unhealthy for Sensitive Groups':'敏感人群不健康','Unhealthy':'不健康','Very Unhealthy':'非常不健康','Hazardous':'危险','Unavailable':'暂不可用'};"
    "const signalLabels={'Good':'良好','Acceptable':'可接受','Elevated':'偏高','High':'高','Very High':'很高','Unavailable':'暂不可用'};"
    "const factorLabels={'PM2.5':'PM2.5','PM10':'PM10','CO2':'二氧化碳','Humidity':'湿度','Unavailable':'未提供'};"
    "const profileLabels={'Fine-Dominant':'细颗粒主导','Mixed':'混合','Coarse-Dominant':'粗颗粒主导','Unavailable':'暂不可用'};"
    "const basisLabels={'EPA AQI (PM2.5 / PM10)':'EPA AQI（PM2.5 / PM10）','EPA AQI unavailable':'EPA AQI 暂不可用','U.S. indoor humidity guidance':'美国室内湿度参考','U.S. indoor ventilation proxy':'美国室内通风参考','U.S. indoor ventilation/humidity guidance':'美国室内通风/湿度参考','EPA AQI with U.S. indoor guidance':'EPA AQI + 美国室内参考','No sensor data':'暂无传感器数据','Unavailable':'未提供'};"
    "function translate(map,value,fallback='未提供'){if(value==null||value==='')return fallback;return map[value]||value;}"
    "function valueOr(value,fallback){return value==null?fallback:value;}"
    "function text(value,fallback='未提供'){return value==null||value===''?fallback:value;}"
    "function num(value,digits=0,unit=''){if(value==null||value==='')return '未提供';const n=Number(value);if(Number.isNaN(n))return '未提供';return `${digits===null?n:n.toFixed(digits)}${unit}`;}"
    "function fmtAge(seconds){if(seconds==null||seconds==='')return '未提供';const sec=Number(seconds);if(Number.isNaN(sec))return '未提供';if(sec<60)return `${sec} 秒`;const min=Math.floor(sec/60);const rem=sec%60;if(min<60)return rem?`${min} 分 ${rem} 秒`:`${min} 分`;const hr=Math.floor(min/60);const m=min%60;return m?`${hr} 小时 ${m} 分`:`${hr} 小时`;}"
    "function kv(label,value){return `<div class='kv'><span>${label}</span><strong>${value}</strong></div>`;}"
    "function metric(label,value,meta=''){return `<div class='metric'><span>${label}</span><strong>${value}</strong>${meta?`<small>${meta}</small>`:''}</div>`;}"
    "function chip(label,tone='neutral'){return `<span class='chip chip-${tone||'neutral'}'>${label}</span>`;}"
    "function summaryBanner(value,detail,tone='neutral'){return `<div class='summary-banner summary-${tone||'neutral'}'><strong>${value}</strong><span>${detail}</span></div>`;}"
    "function localizedOverall(value){return translate(categoryLabels,value,'总体评估暂不可用');}"
    "function localizedSignal(value){return translate(signalLabels,value);}"
    "function localizedFactor(value){return translate(factorLabels,value);}"
    "function localizedProfile(value){return translate(profileLabels,value);}"
    "function localizedBasis(value){return translate(basisLabels,value);}"
    "function localizedLastError(value){if(value==null||value===''||value==='none')return '无';return value"
    ".replaceAll('SCD41: ','SCD41：')"
    ".replaceAll('SGP41: ','SGP41：')"
    ".replaceAll('SPS30: ','SPS30：')"
    ".replaceAll(' | ','；')"
    ".replaceAll('init failed','初始化失败')"
    ".replaceAll('read failed','读取失败')"
    ".replaceAll('ready check failed','就绪检查失败')"
    ".replaceAll('conditioning failed','调理失败')"
    ".replaceAll('FRC ppm must be 400-2000','FRC 参考值必须在 400 到 2000 ppm 之间')"
    ".replaceAll('FRC requires >=3 min in stable target CO2','FRC 需要在稳定目标 CO2 环境下持续至少 3 分钟')"
    ".replaceAll('FRC failed','FRC 执行失败');}"
    "function localizedNote(value){if(value==null||value==='')return '未提供';return value"
    ".replaceAll('Overall rating follows the official EPA AQI based on PM2.5 and PM10.','总体结论直接跟随基于 PM2.5 与 PM10 的官方 EPA AQI。')"
    ".replaceAll('PM AQI is lower, but indoor guidance raises the overall rating.','PM AQI 较低，但室内环境参考将总体结论上调。')"
    ".replaceAll('PM AQI is unavailable, so the rating is limited to CO2 and humidity guidance.','当前没有可用的 PM AQI，因此结论仅参考 CO2 与湿度。')"
    ".replaceAll('CO2 is above the ventilation proxy threshold (1000 ppm).','CO2 已高于通风参考阈值（1000 ppm）。')"
    ".replaceAll('Relative humidity is outside the recommended 30%-60% range.','相对湿度超出建议的 30% 到 60% 区间。')"
    ".replaceAll('VOC/NOx indexes are reported separately and are not EPA AQI pollutants.','VOC/NOx 指数仅作为补充参考，不属于 EPA AQI 污染物。')"
    ".replaceAll('PM1.0, PM4.0, particle counts and typical particle size are supplemental only.','PM1.0、PM4.0、粒子计数和典型粒径仅作补充说明。')"
    ".replaceAll('Not enough AQI-supported measurements are available yet.','当前仍缺少足够的 AQI 支持数据。');}"
    "function localizedParticleNote(value){if(value==null||value==='')return '未提供';const m=/Mass is led by (.*), counts are led by (.*), typical size ([0-9.]+) um\\./.exec(value);if(m){return `质量主导区间：${m[1]}；计数主导区间：${m[2]}；典型粒径：${m[3]} μm。`;}return value;}"
    "function wifiStatus(d){if(d.diag.provisioning_mode)return '等待 BLE 配网';if(d.diag.wifi_connected)return '已连接';return '已保存凭据，当前离线';}"
    "function mqttStatus(d){if(!d.config.mqtt_host)return '未配置';return d.diag.mqtt_connected?'已连接':'已配置，未连接';}"
    "function scd41Status(d){if(!d.diag.scd41_ready)return '未检测到或初始化失败';return d.snapshot.scd41_valid?'在线，正在测量':'在线，等待首包数据';}"
    "function sgp41Status(d){if(!d.diag.sgp41_ready)return '未检测到或初始化失败';if(d.snapshot.sgp41_conditioning)return '在线，NOx 调理中';if(!d.snapshot.sgp41_valid)return '在线，等待首包数据';if(valueOr(d.snapshot.voc_index,0)===0&&valueOr(d.snapshot.nox_index,0)===0)return '在线，算法学习中';return '在线，正在测量';}"
    "function sps30Status(d){if(!d.diag.sps30_ready)return '未检测到或初始化失败';if(d.snapshot.sps30_sleeping)return '在线，休眠中';return d.snapshot.pm_valid?'在线，正在测量':'在线，预热中或等待数据';}"
    "function statusLedStatus(d){if(!d.diag.status_led_ready)return '不可用';return d.diag.status_led_enabled?'已启用':'已关闭';}"
    "function statusLedTone(d){if(!d.diag.status_led_ready)return 'unavailable';return d.diag.status_led_enabled?'good':'neutral';}"
    "function syncActionAvailability(d){const scd41Ready=!!d.diag.scd41_ready;const sps30Ready=!!d.diag.sps30_ready;const ledReady=!!d.diag.status_led_ready;ascOnBtn.disabled=!scd41Ready;ascOffBtn.disabled=!scd41Ready;frcApplyBtn.disabled=!scd41Ready;sps30WakeBtn.disabled=!sps30Ready;sps30SleepBtn.disabled=!sps30Ready;statusLedOnBtn.disabled=!ledReady;statusLedOffBtn.disabled=!ledReady;republishBtn.disabled=!d.diag.mqtt_connected;}"
    "async function getErrorMessage(r,fallback){try{const j=await r.json();return j.message||fallback;}catch(_){try{return await r.text()||fallback;}catch(__){return fallback;}}}"
    "function showNotice(type,message){clearTimeout(noticeTimer);noticeEl.hidden=false;noticeEl.className=`notice notice-${type}`;noticeEl.textContent=message;noticeTimer=setTimeout(()=>{noticeEl.hidden=true;},4200);}"
    "async function apiRequest(url,options,fallback,successMessage){const r=await fetch(url,options);if(!r.ok){showNotice('error',await getErrorMessage(r,fallback));return false;}if(successMessage){showNotice('success',successMessage);}return true;}"
    "function setConfigValues(d){if(!configDirty){for(const k of configFields){const el=configInputs[k];if(el){const value=d.config[k];el.value=value==null?'':value;}}}if(document.activeElement!==frcInput){frcInput.value=d.frc_reference_ppm;}}"
    "function renderHero(d,overallKey){heroBadgesEl.innerHTML=[chip(d.diag.provisioning_mode?'等待配网':'局域网运行',d.diag.provisioning_mode?'elevated':'good'),chip(`总体评估：${localizedOverall(d.snapshot.overall_air_quality)}`,overallKey),chip(`MQTT：${mqttStatus(d)}`,d.diag.mqtt_connected?'good':'neutral'),chip(`RGB 灯：${statusLedStatus(d)}`,statusLedTone(d))].join('');heroMetaEl.innerHTML=[kv('设备名称',text(d.config.device_name)),kv('设备 ID',text(d.diag.device_id)),kv('固件版本',text(d.diag.firmware_version)),kv('IP 地址',text(d.diag.ip_addr,'暂无'))].join('');}"
    "function renderOverview(d){overviewEl.innerHTML=[kv('运行模式',d.diag.provisioning_mode?'BLE 配网':'局域网运行'),kv('Wi-Fi 信号',d.diag.wifi_connected?`${d.diag.wifi_rssi} dBm`:'暂无'),kv('发布间隔',num(d.config.publish_interval_sec,0,' 秒')),kv('Discovery 前缀',text(d.config.discovery_prefix)),kv('主题根路径',text(d.config.topic_root)),kv('FRC 参考值',num(d.frc_reference_ppm,0,' ppm')),kv('样本年龄',fmtAge(d.snapshot.sample_age_sec)),kv('最近错误',localizedLastError(d.diag.last_error))].join('');}"
    "function renderServices(d){servicesEl.innerHTML=[kv('Wi-Fi',wifiStatus(d)),kv('MQTT',mqttStatus(d)),kv('SCD41',scd41Status(d)),kv('SGP41',sgp41Status(d)),kv('SPS30',sps30Status(d)),kv('RGB 状态灯',statusLedStatus(d)),kv('传感器总体',d.diag.sensors_ready?'已就绪':'准备中')].join('');}"
    "function renderAir(d,overallKey){const overall=localizedOverall(d.snapshot.overall_air_quality);const basis=localizedBasis(d.snapshot.overall_air_quality_basis);const note=localizedNote(d.snapshot.overall_air_quality_note);const usAqi=d.snapshot.us_aqi!=null?`${d.snapshot.us_aqi} · ${translate(categoryLabels,d.snapshot.us_aqi_level)}`:'暂不可用';airSummaryEl.innerHTML=summaryBanner(`总体评估：${overall}`,`${basis}。${note}`,overallKey);airMetricsEl.innerHTML=[metric('US AQI',usAqi,text(translate(categoryLabels,d.snapshot.us_aqi_level),'暂无等级')),metric('主要污染物',translate(factorLabels,d.snapshot.us_aqi_primary_pollutant),text(localizedFactor(d.snapshot.overall_air_quality_driver),'未提供')),metric('CO2',num(d.snapshot.co2_ppm,0,' ppm'),localizedSignal(d.snapshot.co2_rating)),metric('温度',num(d.snapshot.temperature_c,1,' °C'),'SCD41 实时测量'),metric('湿度',num(d.snapshot.humidity_rh,1,' %'),'建议区间 30% 到 60%'),metric('VOC 指数',num(d.snapshot.voc_index,0,''),localizedSignal(d.snapshot.voc_rating)),metric('NOx 指数',num(d.snapshot.nox_index,0,''),localizedSignal(d.snapshot.nox_rating)),metric('样本年龄',fmtAge(d.snapshot.sample_age_sec),'最新一次有效采样到现在')].join('');}"
    "function renderParticles(d){const keyMap={fine:'fine',mixed:'mixed',coarse:'coarse',unavailable:'neutral'};const profileKey=keyMap[d.snapshot.particle_profile_key]||'neutral';particleSummaryEl.innerHTML=summaryBanner(`颗粒物画像：${localizedProfile(d.snapshot.particle_profile)}`,localizedParticleNote(d.snapshot.particle_profile_note),profileKey);particleMetricsEl.innerHTML=[kv('PM1.0',num(d.snapshot.pm1_0,1,' µg/m³')),kv('PM2.5',num(d.snapshot.pm2_5,1,' µg/m³')),kv('PM4.0',num(d.snapshot.pm4_0,1,' µg/m³')),kv('PM10',num(d.snapshot.pm10_0,1,' µg/m³')),kv('>0.5 µm 粒子数',num(d.snapshot.particles_0_5um,1,' #/cm³')),kv('>1.0 µm 粒子数',num(d.snapshot.particles_1_0um,1,' #/cm³')),kv('>2.5 µm 粒子数',num(d.snapshot.particles_2_5um,1,' #/cm³')),kv('>4.0 µm 粒子数',num(d.snapshot.particles_4_0um,1,' #/cm³')),kv('>10 µm 粒子数',num(d.snapshot.particles_10_0um,1,' #/cm³')),kv('典型粒径',num(d.snapshot.typical_particle_size_um,2,' µm'))].join('');}"
    "async function fetchStatus(){const r=await fetch('/api/status');if(!r.ok)return;const d=await r.json();const overallKey=d.snapshot.overall_air_quality_key||'neutral';renderHero(d,overallKey);renderOverview(d);renderServices(d);renderAir(d,overallKey);renderParticles(d);syncActionAvailability(d);setConfigValues(d);}"
    "async function saveConfig(){const payload={device_name:configInputs.device_name.value,wifi_ssid:configInputs.wifi_ssid.value,wifi_password:configInputs.wifi_password.value,mqtt_host:configInputs.mqtt_host.value,mqtt_port:Number(configInputs.mqtt_port.value),mqtt_username:configInputs.mqtt_username.value,mqtt_password:configInputs.mqtt_password.value,discovery_prefix:configInputs.discovery_prefix.value,topic_root:configInputs.topic_root.value,publish_interval_sec:Number(configInputs.publish_interval_sec.value),scd41_altitude_m:Number(configInputs.scd41_altitude_m.value),scd41_temp_offset_c:Number(configInputs.scd41_temp_offset_c.value)};const ok=await apiRequest('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)},'配置保存失败','配置已保存，设备将自动重启。');if(ok){configDirty=false;setTimeout(fetchStatus,800);}}"
    "async function toggleAsc(enabled){if(await apiRequest('/api/action/scd41-asc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled})},'SCD41 ASC 操作失败',enabled?'已开启 ASC。':'已关闭 ASC。')){fetchStatus();}}"
    "async function toggleSps30(sleep){if(await apiRequest('/api/action/sps30-sleep',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({sleep})},'SPS30 操作失败',sleep?'已让 SPS30 进入休眠。':'已唤醒 SPS30。')){fetchStatus();}}"
    "async function toggleStatusLed(enabled){if(await apiRequest('/api/action/status-led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled})},'RGB 状态灯操作失败',enabled?'已打开 RGB 状态灯。':'已关闭 RGB 状态灯。')){fetchStatus();}}"
    "async function applyFrc(){if(await apiRequest('/api/action/apply-frc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ppm:Number(frcInput.value)})},'FRC 校准请求失败','已提交 FRC 校准请求。')){fetchStatus();}}"
    "async function republishDiscovery(){if(await apiRequest('/api/action/republish-discovery',{method:'POST'},'重新发布 Discovery 失败','已触发重新发布 Discovery。')){fetchStatus();}}"
    "async function restartDevice(){if(await apiRequest('/api/action/restart',{method:'POST'},'重启请求失败','已请求设备重启。')){}}"
    "async function factoryReset(){if(confirm('确定要恢复出厂设置吗？已保存的配置会被清除。')){await apiRequest('/api/action/factory-reset',{method:'POST'},'恢复出厂设置失败','已请求恢复出厂设置，设备将重新启动。');}}"
    "async function uploadFirmware(){const file=firmwareInput.files[0];if(!file){showNotice('info','请先选择一个固件文件。');return;}const ok=await apiRequest('/api/ota',{method:'POST',body:file},'固件上传失败');if(ok){showNotice('success','固件上传完成，设备将自动重启。');}}"
    "for(const key of configFields){const el=document.getElementById(key);if(el){el.addEventListener('input',()=>{configDirty=true;});}}"
    "fetchStatus();setInterval(fetchStatus,4000);"
    "</script></body></html>";

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, TAG, "json render failed");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

static char *read_body(httpd_req_t *req)
{
    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return NULL;
        }
        received += ret;
    }
    body[received] = '\0';
    return body;
}

static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "json alloc failed");
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", message);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_ok_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t send_ok_restart_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"restart\":true}");
}

static esp_err_t parse_json_request(httpd_req_t *req, cJSON **json_out)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error_json(req, "400 Bad Request", "请求体缺失或被截断");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return send_error_json(req, "400 Bad Request", "JSON 格式无效");
    }

    *json_out = json;
    return ESP_OK;
}

static bool number_in_range(cJSON *item, double min, double max)
{
    return cJSON_IsNumber(item) && !isnan(item->valuedouble) &&
           item->valuedouble >= min && item->valuedouble <= max;
}

static bool whole_number_in_range(cJSON *item, long min, long max)
{
    return number_in_range(item, (double)min, (double)max) &&
           floor(item->valuedouble) == item->valuedouble;
}

static void fill_status(sensor_snapshot_t *snapshot, device_diag_t *diag, device_config_t *config, uint16_t *frc_ppm)
{
    memset(snapshot, 0, sizeof(*snapshot));
    memset(diag, 0, sizeof(*diag));
    memset(config, 0, sizeof(*config));
    *frc_ppm = 400;
    if (s_ctx.callbacks.get_status != NULL) {
        s_ctx.callbacks.get_status(snapshot, diag, config, frc_ppm, s_ctx.user_ctx);
    }
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot;
    device_diag_t diag;
    device_config_t config;
    uint16_t frc_ppm;
    fill_status(&snapshot, &diag, &config, &frc_ppm);
    air_quality_assessment_t assessment = {0};
    air_quality_compute_overall_assessment(&snapshot, &assessment);
    air_quality_particle_insight_t particle = {0};
    air_quality_compute_particle_insight(&snapshot, &particle);
    int64_t now_ms = esp_timer_get_time() / 1000;

    cJSON *root = cJSON_CreateObject();
    cJSON *diag_json = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddBoolToObject(diag_json, "provisioning_mode", diag.provisioning_mode);
    cJSON_AddBoolToObject(diag_json, "wifi_connected", diag.wifi_connected);
    cJSON_AddBoolToObject(diag_json, "mqtt_connected", diag.mqtt_connected);
    cJSON_AddBoolToObject(diag_json, "sensors_ready", diag.sensors_ready);
    cJSON_AddBoolToObject(diag_json, "scd41_ready", diag.scd41_ready);
    cJSON_AddBoolToObject(diag_json, "sgp41_ready", diag.sgp41_ready);
    cJSON_AddBoolToObject(diag_json, "sps30_ready", diag.sps30_ready);
    cJSON_AddBoolToObject(diag_json, "status_led_ready", diag.status_led_ready);
    cJSON_AddBoolToObject(diag_json, "status_led_enabled", diag.status_led_enabled);
    cJSON_AddNumberToObject(diag_json, "wifi_rssi", diag.wifi_rssi);
    cJSON_AddNumberToObject(diag_json, "uptime_sec", diag.uptime_sec);
    cJSON_AddNumberToObject(diag_json, "heap_free", diag.heap_free);
    cJSON_AddStringToObject(diag_json, "ip_addr", diag.ip_addr);
    cJSON_AddStringToObject(diag_json, "ap_ssid", diag.ap_ssid);
    cJSON_AddStringToObject(diag_json, "device_id", diag.device_id);
    cJSON_AddStringToObject(diag_json, "firmware_version", diag.firmware_version);
    cJSON_AddStringToObject(diag_json, "last_error", diag.last_error[0] ? diag.last_error : "none");

    cJSON *snapshot_json = cJSON_AddObjectToObject(root, "snapshot");
    cJSON_AddBoolToObject(snapshot_json, "scd41_valid", snapshot.scd41_valid);
    cJSON_AddBoolToObject(snapshot_json, "sgp41_valid", snapshot.sgp41_valid);
    cJSON_AddBoolToObject(snapshot_json, "sgp41_conditioning", snapshot.sgp41_conditioning);
    cJSON_AddBoolToObject(snapshot_json, "pm_valid", snapshot.pm_valid);
    if (snapshot.scd41_valid) {
        cJSON_AddNumberToObject(snapshot_json, "co2_ppm", snapshot.co2_ppm);
        cJSON_AddStringToObject(snapshot_json, "co2_rating",
                                air_quality_signal_level_label(air_quality_rate_co2(snapshot.co2_ppm)));
        cJSON_AddNumberToObject(snapshot_json, "temperature_c", snapshot.temperature_c);
        cJSON_AddNumberToObject(snapshot_json, "humidity_rh", snapshot.humidity_rh);
    } else {
        cJSON_AddNullToObject(snapshot_json, "co2_ppm");
        cJSON_AddNullToObject(snapshot_json, "co2_rating");
        cJSON_AddNullToObject(snapshot_json, "temperature_c");
        cJSON_AddNullToObject(snapshot_json, "humidity_rh");
    }
    if (snapshot.sgp41_valid && !snapshot.sgp41_conditioning) {
        cJSON_AddNumberToObject(snapshot_json, "voc_index", snapshot.voc_index);
        cJSON_AddStringToObject(snapshot_json, "voc_rating",
                                air_quality_signal_level_label(air_quality_rate_voc_index(snapshot.voc_index)));
        cJSON_AddNumberToObject(snapshot_json, "nox_index", snapshot.nox_index);
        cJSON_AddStringToObject(snapshot_json, "nox_rating",
                                air_quality_signal_level_label(air_quality_rate_nox_index(snapshot.nox_index)));
    } else {
        cJSON_AddNullToObject(snapshot_json, "voc_index");
        cJSON_AddNullToObject(snapshot_json, "voc_rating");
        cJSON_AddNullToObject(snapshot_json, "nox_index");
        cJSON_AddNullToObject(snapshot_json, "nox_rating");
    }
    if (snapshot.pm_valid) {
        cJSON_AddNumberToObject(snapshot_json, "pm1_0", snapshot.pm1_0);
        cJSON_AddNumberToObject(snapshot_json, "pm2_5", snapshot.pm2_5);
        cJSON_AddNumberToObject(snapshot_json, "pm4_0", snapshot.pm4_0);
        cJSON_AddNumberToObject(snapshot_json, "pm10_0", snapshot.pm10_0);
        cJSON_AddStringToObject(snapshot_json, "particle_profile",
                                air_quality_particle_profile_label(particle.profile));
        cJSON_AddStringToObject(snapshot_json, "particle_profile_key",
                                air_quality_particle_profile_key(particle.profile));
        cJSON_AddStringToObject(snapshot_json, "particle_profile_note",
                                particle.note[0] ? particle.note : "Unavailable");
        cJSON_AddNumberToObject(snapshot_json, "particles_0_5um", snapshot.particles_0_5um);
        cJSON_AddNumberToObject(snapshot_json, "particles_1_0um", snapshot.particles_1_0um);
        cJSON_AddNumberToObject(snapshot_json, "particles_2_5um", snapshot.particles_2_5um);
        cJSON_AddNumberToObject(snapshot_json, "particles_4_0um", snapshot.particles_4_0um);
        cJSON_AddNumberToObject(snapshot_json, "particles_10_0um", snapshot.particles_10_0um);
        cJSON_AddNumberToObject(snapshot_json, "typical_particle_size_um", snapshot.typical_particle_size_um);
    } else {
        cJSON_AddNullToObject(snapshot_json, "pm1_0");
        cJSON_AddNullToObject(snapshot_json, "pm2_5");
        cJSON_AddNullToObject(snapshot_json, "pm4_0");
        cJSON_AddNullToObject(snapshot_json, "pm10_0");
        cJSON_AddNullToObject(snapshot_json, "particle_profile");
        cJSON_AddNullToObject(snapshot_json, "particle_profile_key");
        cJSON_AddNullToObject(snapshot_json, "particle_profile_note");
        cJSON_AddNullToObject(snapshot_json, "particles_0_5um");
        cJSON_AddNullToObject(snapshot_json, "particles_1_0um");
        cJSON_AddNullToObject(snapshot_json, "particles_2_5um");
        cJSON_AddNullToObject(snapshot_json, "particles_4_0um");
        cJSON_AddNullToObject(snapshot_json, "particles_10_0um");
        cJSON_AddNullToObject(snapshot_json, "typical_particle_size_um");
    }
    if (snapshot.updated_at_ms > 0 && now_ms >= snapshot.updated_at_ms) {
        cJSON_AddNumberToObject(snapshot_json, "sample_age_sec", (now_ms - snapshot.updated_at_ms) / 1000);
    } else {
        cJSON_AddNullToObject(snapshot_json, "sample_age_sec");
    }
    cJSON_AddBoolToObject(snapshot_json, "sps30_sleeping", snapshot.sps30_sleeping);
    if (assessment.us_aqi.valid) {
        cJSON_AddNumberToObject(snapshot_json, "us_aqi", assessment.us_aqi.aqi);
        cJSON_AddStringToObject(snapshot_json, "us_aqi_level", air_quality_category_label(assessment.us_aqi.category));
        cJSON_AddStringToObject(snapshot_json, "us_aqi_level_key", air_quality_category_key(assessment.us_aqi.category));
        cJSON_AddStringToObject(snapshot_json, "us_aqi_primary_pollutant",
                                air_quality_pollutant_label(assessment.us_aqi.dominant_pollutant));
    } else {
        cJSON_AddNullToObject(snapshot_json, "us_aqi");
        cJSON_AddNullToObject(snapshot_json, "us_aqi_level");
        cJSON_AddStringToObject(snapshot_json, "us_aqi_level_key", air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(snapshot_json, "us_aqi_primary_pollutant");
    }
    if (assessment.valid) {
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality", air_quality_category_label(assessment.category));
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality_key", air_quality_category_key(assessment.category));
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality_driver",
                                air_quality_factor_label(assessment.dominant_factor));
    } else {
        cJSON_AddNullToObject(snapshot_json, "overall_air_quality");
        cJSON_AddStringToObject(snapshot_json, "overall_air_quality_key",
                                air_quality_category_key(AIR_QUALITY_CATEGORY_UNKNOWN));
        cJSON_AddNullToObject(snapshot_json, "overall_air_quality_driver");
    }
    cJSON_AddStringToObject(snapshot_json, "overall_air_quality_basis",
                            assessment.basis[0] ? assessment.basis : "Unavailable");
    cJSON_AddStringToObject(snapshot_json, "overall_air_quality_note",
                            assessment.note[0] ? assessment.note : "Unavailable");

    cJSON *config_json = cJSON_AddObjectToObject(root, "config");
    cJSON_AddStringToObject(config_json, "device_name", config.device_name);
    cJSON_AddStringToObject(config_json, "wifi_ssid", config.wifi_ssid);
    cJSON_AddStringToObject(config_json, "wifi_password", config.wifi_password);
    cJSON_AddStringToObject(config_json, "mqtt_host", config.mqtt_host);
    cJSON_AddNumberToObject(config_json, "mqtt_port", config.mqtt_port);
    cJSON_AddStringToObject(config_json, "mqtt_username", config.mqtt_username);
    cJSON_AddStringToObject(config_json, "mqtt_password", config.mqtt_password);
    cJSON_AddStringToObject(config_json, "discovery_prefix", config.discovery_prefix);
    cJSON_AddStringToObject(config_json, "topic_root", config.topic_root);
    cJSON_AddNumberToObject(config_json, "publish_interval_sec", config.publish_interval_sec);
    cJSON_AddNumberToObject(config_json, "scd41_altitude_m", config.scd41_altitude_m);
    cJSON_AddNumberToObject(config_json, "scd41_temp_offset_c", config.scd41_temp_offset_c);
    cJSON_AddNumberToObject(root, "frc_reference_ppm", frc_ppm);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot;
    device_diag_t diag;
    device_config_t config;
    uint16_t frc_ppm;
    fill_status(&snapshot, &diag, &config, &frc_ppm);

    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *item = NULL;
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "device_name")) && cJSON_IsString(item)) {
        strlcpy(config.device_name, item->valuestring, sizeof(config.device_name));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "wifi_ssid")) && cJSON_IsString(item)) {
        strlcpy(config.wifi_ssid, item->valuestring, sizeof(config.wifi_ssid));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "wifi_password")) && cJSON_IsString(item)) {
        strlcpy(config.wifi_password, item->valuestring, sizeof(config.wifi_password));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_host")) && cJSON_IsString(item)) {
        strlcpy(config.mqtt_host, item->valuestring, sizeof(config.mqtt_host));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_port")) != NULL) {
        if (!whole_number_in_range(item, MQTT_PORT_MIN, MQTT_PORT_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "MQTT 端口必须在 1 到 65535 之间");
        }
        config.mqtt_port = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_username")) && cJSON_IsString(item)) {
        strlcpy(config.mqtt_username, item->valuestring, sizeof(config.mqtt_username));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "mqtt_password")) && cJSON_IsString(item)) {
        strlcpy(config.mqtt_password, item->valuestring, sizeof(config.mqtt_password));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "discovery_prefix")) && cJSON_IsString(item)) {
        strlcpy(config.discovery_prefix, item->valuestring, sizeof(config.discovery_prefix));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "topic_root")) && cJSON_IsString(item)) {
        strlcpy(config.topic_root, item->valuestring, sizeof(config.topic_root));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "publish_interval_sec")) != NULL) {
        if (!whole_number_in_range(item, PUBLISH_INTERVAL_MIN, PUBLISH_INTERVAL_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "发布间隔必须在 5 到 60 秒之间");
        }
        config.publish_interval_sec = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "scd41_altitude_m")) != NULL) {
        if (!whole_number_in_range(item, SCD41_ALTITUDE_MIN, SCD41_ALTITUDE_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "SCD41 海拔补偿必须在 0 到 3000 米之间");
        }
        config.scd41_altitude_m = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(json, "scd41_temp_offset_c")) != NULL) {
        if (!number_in_range(item, SCD41_TEMP_OFFSET_MIN, SCD41_TEMP_OFFSET_MAX)) {
            cJSON_Delete(json);
            return send_error_json(req, "400 Bad Request", "SCD41 温度偏移必须在 0 到 20 摄氏度之间");
        }
        config.scd41_temp_offset_c = (float)item->valuedouble;
    }
    cJSON_Delete(json);

    if (s_ctx.callbacks.save_config != NULL) {
        ESP_RETURN_ON_ERROR(s_ctx.callbacks.save_config(&config, s_ctx.user_ctx), TAG, "save config failed");
    }

    return send_ok_restart_json(req);
}

static esp_err_t simple_action_handler(httpd_req_t *req, void (*action)(void *))
{
    if (action != NULL) {
        action(s_ctx.user_ctx);
    }
    return send_ok_json(req);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    return simple_action_handler(req, s_ctx.callbacks.request_restart);
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    return simple_action_handler(req, s_ctx.callbacks.request_factory_reset);
}

static esp_err_t republish_handler(httpd_req_t *req)
{
    if (s_ctx.callbacks.request_republish_discovery != NULL) {
        esp_err_t err = s_ctx.callbacks.request_republish_discovery(s_ctx.user_ctx);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "MQTT 未连接，无法重新发布 Discovery");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "republish discovery action failed");
    }
    return send_ok_json(req);
}

static esp_err_t scd41_asc_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "缺少有效的 enabled 布尔字段");
    }

    if (s_ctx.callbacks.request_set_scd41_asc != NULL) {
        esp_err_t err = s_ctx.callbacks.request_set_scd41_asc(cJSON_IsTrue(enabled), s_ctx.user_ctx);
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "SCD41 当前不可用");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "scd41 asc action failed");
        return send_ok_json(req);
    }

    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t sps30_sleep_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *sleep = cJSON_GetObjectItemCaseSensitive(json, "sleep");
    if (!cJSON_IsBool(sleep)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "缺少有效的 sleep 布尔字段");
    }

    if (s_ctx.callbacks.request_set_sps30_sleep != NULL) {
        esp_err_t err = s_ctx.callbacks.request_set_sps30_sleep(cJSON_IsTrue(sleep), s_ctx.user_ctx);
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "SPS30 当前不可用");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "sps30 sleep action failed");
        return send_ok_json(req);
    }

    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t status_led_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "缺少有效的 enabled 布尔字段");
    }

    if (s_ctx.callbacks.request_set_status_led != NULL) {
        esp_err_t err = s_ctx.callbacks.request_set_status_led(cJSON_IsTrue(enabled), s_ctx.user_ctx);
        cJSON_Delete(json);
        if (err == ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "409 Conflict", "RGB 状态灯当前不可用");
        }
        ESP_RETURN_ON_ERROR(err, TAG, "status led action failed");
        return send_ok_json(req);
    }

    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t frc_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    esp_err_t parse_err = parse_json_request(req, &json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    cJSON *ppm = cJSON_GetObjectItemCaseSensitive(json, "ppm");
    if (!whole_number_in_range(ppm, 400, 2000)) {
        cJSON_Delete(json);
        return send_error_json(req, "400 Bad Request", "FRC 参考值必须在 400 到 2000 ppm 之间");
    }
    if (s_ctx.callbacks.request_apply_frc != NULL) {
        esp_err_t err = s_ctx.callbacks.request_apply_frc((uint16_t)ppm->valuedouble, s_ctx.user_ctx);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_error_json(req, "409 Conflict", "当前不满足 SCD41 FRC 的数据手册前置条件");
        }
    }
    cJSON_Delete(json);
    return send_ok_json(req);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_FAIL, TAG, "no ota partition");

    esp_ota_handle_t ota_handle = 0;
    ESP_RETURN_ON_ERROR(esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle), TAG, "ota begin failed");

    char chunk[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int read = httpd_req_recv(req, chunk, remaining > (int)sizeof(chunk) ? (int)sizeof(chunk) : remaining);
        if (read <= 0) {
            esp_ota_abort(ota_handle);
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(esp_ota_write(ota_handle, chunk, read), TAG, "ota write failed");
        remaining -= read;
    }

    ESP_RETURN_ON_ERROR(esp_ota_end(ota_handle), TAG, "ota end failed");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "set boot partition failed");
    if (s_ctx.callbacks.request_restart != NULL) {
        s_ctx.callbacks.request_restart(s_ctx.user_ctx);
    }
    return send_ok_restart_json(req);
}

esp_err_t provisioning_web_start(const char *device_id,
                                 const provisioning_web_callbacks_t *callbacks,
                                 void *user_ctx)
{
    provisioning_web_stop();
    memset(&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.device_id, device_id, sizeof(s_ctx.device_id));
    if (callbacks != NULL) {
        s_ctx.callbacks = *callbacks;
    }
    s_ctx.user_ctx = user_ctx;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, "httpd_start failed");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler, .user_ctx = NULL},
        {.uri = "/api/action/restart", .method = HTTP_POST, .handler = restart_handler, .user_ctx = NULL},
        {.uri = "/api/action/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler, .user_ctx = NULL},
        {.uri = "/api/action/republish-discovery", .method = HTTP_POST, .handler = republish_handler, .user_ctx = NULL},
        {.uri = "/api/action/scd41-asc", .method = HTTP_POST, .handler = scd41_asc_handler, .user_ctx = NULL},
        {.uri = "/api/action/sps30-sleep", .method = HTTP_POST, .handler = sps30_sleep_handler, .user_ctx = NULL},
        {.uri = "/api/action/status-led", .method = HTTP_POST, .handler = status_led_handler, .user_ctx = NULL},
        {.uri = "/api/action/apply-frc", .method = HTTP_POST, .handler = frc_handler, .user_ctx = NULL},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler, .user_ctx = NULL},
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_ctx.server, &uris[i]), TAG, "uri register failed");
    }
    return ESP_OK;
}

void provisioning_web_stop(void)
{
    if (s_ctx.server != NULL) {
        httpd_stop(s_ctx.server);
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}
