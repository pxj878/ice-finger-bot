/*
 * ============================================================
 *  制冰机手指机器人  Ice Finger Bot  (ESP32-S3)
 * ------------------------------------------------------------
 *  功能:
 *    - SG90 舵机模拟手指, 按压制冰机电源/开关按钮
 *    - WiFi 配网 (AP 热点 + 手机网页配置)
 *    - 网页端配置: 舵机角度、按住时长、WiFi、时区
 *    - 定时任务: 开机 / 关机 / 切换, 支持按星期重复 + 单次
 *    - REST API: 给微信小程序 / 后续扩展使用
 *  硬件:
 *    GPIO4 -> SG90 信号(橙)   SG90 5V 独立供电, GND 共地
 *    GPIO0 -> 板载 BOOT 键, 可当手动按键 (可选)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <math.h>

#include "esp_wifi.h"  // 用于给 STA 打开 PMF(802.11w) 兼容

#include "web_ui.h"

#define FW_VERSION "1.0.0"

// ---------------- 引脚定义 ----------------
// SG90 信号线。不同板子丝印不一样（DevKitC-1 印 "4"，Feather 印 "A3"），
// 需要换脚时不用改代码：pio run -e esp32-s3-devkitc-1 -DPIN_SERVO=5
#ifndef PIN_SERVO
#define PIN_SERVO 4
#endif
// 手动按键（板载 BOOT 键，低电平有效）：C3 是 GPIO9，S3 是 GPIO0
// S3 环境在 platformio.ini 里用 -DPIN_BTN=0 覆盖
#ifndef PIN_BTN
#define PIN_BTN 9
#endif

// ---------------- 常量 ----------------
static const char *AP_SSID  = "IceFingerBot";  // 配网热点名
static const char *AP_PASS  = "icebot1234";    // 配网热点密码
static const char *DEF_HOST = "icebot";        // mDNS: http://icebot.local

#define MAX_TASKS    16
#define SERVO_CH     0
#define SERVO_FREQ   50
// ESP32-C3 的 LEDC 最大 14 bit 分辨率（S3 支持更高），统一用 14 bit 兼容两种芯片
#define SERVO_RES    14
#define SERVO_MAXDUTY ((1UL << SERVO_RES) - 1)
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500
#define MOVE_MS      260  // 手指压下的过渡时间

// ---------------- 数据结构 ----------------
struct Config {
  char     ssid[33];
  char     pass[65];
  char     host[32];
  bool     schedEnabled;  // 定时总开关
  uint8_t  restAngle;     // 抬起角度
  uint8_t  pressAngle;    // 压下角度
  uint16_t holdMs;        // 按住时长
  int16_t  tzMin;         // 时区(分钟, 东八区=480)
  bool     powerOn;       // 逻辑电源状态(仅记录, 无反馈)
  bool     apAlways;      // 是否常开配网热点

  // ---- 云端（微信云开发 HTTP 访问服务 / 任意自建 HTTP 接口）----
  bool     cloudOn;       // 是否启用云轮询
  char     cloudUrl[160]; // 完整 URL，例如 https://xxx.service.tcloudbase.com/icebot
  char     cloudToken[48];// 简单鉴权 token，和云函数里保持一致
  uint16_t pollSec;       // 轮询间隔（秒），3~60

  // ---- 静态 IP（可选，不依赖路由器 DHCP 保留）----
  bool     staticIp;
  char     ip[16];
  char     gw[16];
  char     mask[16];
  char     dns[16];

  // ---- 省电 ----
  bool     powerSave;    // 省电模式总开关
  uint16_t servoIdleSec; // 舵机静止多久后停 PWM（0=不停）
  uint16_t apAutoOffSec; // 联网后多久关热点（0=不关）
  uint16_t cpuMhz;       // CPU 频率 80/160/240

  // ---- 语音音箱（巴法云 -> 米家/小爱同学）----
  bool     voiceOn;
  uint8_t  voiceProto;     // 0=MQTT(bemfa.com:9501)  1=TCP(bemfa.com:8344)
  char     bemfaUid[40];   // 巴法云私钥
  char     bemfaTopic[33]; // 主题名，后三位 006 = 开关设备（例：bingji006）
};

struct Task {
  bool    enabled;
  uint8_t hour;
  uint8_t minute;
  uint8_t days;    // bit0=周一 ... bit6=周日
  uint8_t action;  // 0=开机 1=关机 2=切换
  bool    once;    // 单次任务
  char    lastRun[20];
};

Config      cfg;
Task        tasks[MAX_TASKS];
int         taskCount = 0;
Preferences prefs;
WebServer   server(80);

// 舵机状态机
enum ServoPhase { PH_IDLE = 0, PH_MOVE, PH_HOLD, PH_MANUAL };
ServoPhase phase        = PH_IDLE;
uint32_t   phaseAt      = 0;
int        pendingPress = 0;
int        lastAngle    = -1;
// 动作过程中的起止角度（按下/回位/手动都复用同一套插值）
int        fromAngle      = 0;
int        toAngle        = 0;
ServoPhase phaseAfterMove = PH_IDLE;

uint32_t rebootAt = 0;

// ---------------- 日志环形缓冲 ----------------
#define LOG_N 24
String logBuf[LOG_N];
int    logHead = 0, logCount = 0;

void logAdd(const char *fmt, ...) {
  char    buf[110];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  logBuf[logHead] = String(buf);
  logHead         = (logHead + 1) % LOG_N;
  if (logCount < LOG_N) logCount++;
  Serial.printf("[%8lu] %s\n", (unsigned long)millis(), buf);
}

// ---------------- 工具 ----------------
void copyStr(char *dst, size_t n, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

bool timeValid() { return time(nullptr) > 1700000000UL; }

void applyTz() {
  int  m    = cfg.tzMin;
  int  sign = (m >= 0) ? -1 : 1;  // POSIX TZ 符号与 UTC 偏移相反
  int  am   = (m < 0) ? -m : m;
  char buf[24];
  snprintf(buf, sizeof buf, "UTC%s%d:%02d", sign < 0 ? "-" : "+", am / 60, am % 60);
  setenv("TZ", buf, 1);
  tzset();
}

void startNtp() {
  configTime(0, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "cn.pool.ntp.org");
  applyTz();
  logAdd("NTP 对时已启动");
}

// ---------------- 配置持久化 ----------------
String loadStr(const char *key) {
  if (!prefs.isKey(key)) return String();
  return prefs.getString(key, "");
}

void cfgDefaults() {
  memset(&cfg, 0, sizeof cfg);
  copyStr(cfg.host, sizeof cfg.host, DEF_HOST);
  cfg.schedEnabled = true;
  cfg.restAngle    = 25;
  cfg.pressAngle   = 75;
  cfg.holdMs       = 450;
  cfg.tzMin        = 480;
  cfg.powerOn      = false;
  cfg.apAlways     = true;
  cfg.cloudOn      = false;
  cfg.pollSec      = 5;
  cfg.staticIp     = false;
  copyStr(cfg.mask, sizeof cfg.mask, "255.255.255.0");
  copyStr(cfg.dns, sizeof cfg.dns, "223.5.5.5");
  cfg.powerSave    = false;
  cfg.servoIdleSec = 5;
  cfg.apAutoOffSec = 60;
  cfg.cpuMhz       = 240;
  cfg.voiceOn      = false;
  cfg.voiceProto   = 0;  // 0=MQTT, 1=TCP

#ifdef DEFAULT_WIFI_SSID
  // 编译期预置 WiFi（工厂预配置 / 调试用，见 platformio.local.ini）
  copyStr(cfg.ssid, sizeof cfg.ssid, DEFAULT_WIFI_SSID);
  copyStr(cfg.pass, sizeof cfg.pass, DEFAULT_WIFI_PASS);
#endif
}

void cfgToJson(JsonObject o, bool withPass = false) {
  o["ssid"]         = cfg.ssid;
  o["hasPass"]      = (strlen(cfg.pass) > 0);
  // 密码只在写 NVS 时序列化，HTTP 响应里不下发（避免泄露）
  if (withPass) o["pass"] = cfg.pass;
  o["host"]         = cfg.host;
  o["schedEnabled"] = cfg.schedEnabled;
  o["restAngle"]    = cfg.restAngle;
  o["pressAngle"]   = cfg.pressAngle;
  o["holdMs"]       = cfg.holdMs;
  o["tzMin"]        = cfg.tzMin;
  o["powerOn"]      = cfg.powerOn;
  o["apAlways"]     = cfg.apAlways;

  o["cloudOn"]      = cfg.cloudOn;
  o["cloudUrl"]     = cfg.cloudUrl;
  o["hasCloudToken"] = (strlen(cfg.cloudToken) > 0);
  if (withPass) o["cloudToken"] = cfg.cloudToken;
  o["pollSec"]      = cfg.pollSec;

  o["staticIp"]     = cfg.staticIp;
  o["ip"]           = cfg.ip;
  o["gw"]           = cfg.gw;
  o["mask"]         = (strlen(cfg.mask) ? cfg.mask : "255.255.255.0");
  o["dns"]          = cfg.dns;

  o["powerSave"]    = cfg.powerSave;
  o["servoIdleSec"] = cfg.servoIdleSec;
  o["apAutoOffSec"] = cfg.apAutoOffSec;
  o["cpuMhz"]       = cfg.cpuMhz;

  o["voiceOn"]      = cfg.voiceOn;
  o["voiceProto"]   = cfg.voiceProto;
  o["bemfaTopic"]   = cfg.bemfaTopic;
  o["hasBemfaUid"]  = (strlen(cfg.bemfaUid) > 0);
  if (withPass) o["bemfaUid"] = cfg.bemfaUid;
}

void saveCfg() {
  DynamicJsonDocument d(1024);
  cfgToJson(d.to<JsonObject>(), true);  // 写 NVS 时要带密码
  String s;
  serializeJson(d, s);
  prefs.putString("cfg", s);
}

void loadCfg() {
  cfgDefaults();
  String s = loadStr("cfg");
  if (s.length() == 0) return;
  DynamicJsonDocument d(1024);
  if (deserializeJson(d, s)) {
    logAdd("配置解析失败, 使用默认值");
    return;
  }
  JsonObject o = d.as<JsonObject>();
  if (o.containsKey("ssid")) copyStr(cfg.ssid, sizeof cfg.ssid, o["ssid"] | "");
  if (o.containsKey("pass")) copyStr(cfg.pass, sizeof cfg.pass, o["pass"] | "");
  if (o.containsKey("host")) copyStr(cfg.host, sizeof cfg.host, o["host"] | DEF_HOST);
  cfg.schedEnabled = o["schedEnabled"] | true;
  cfg.restAngle    = (uint8_t)constrain((int)(o["restAngle"] | 25), 0, 180);
  cfg.pressAngle   = (uint8_t)constrain((int)(o["pressAngle"] | 75), 0, 180);
  cfg.holdMs       = (uint16_t)constrain((int)(o["holdMs"] | 450), 50, 5000);
  cfg.tzMin        = (int16_t)constrain((int)(o["tzMin"] | 480), -720, 840);
  cfg.powerOn      = o["powerOn"] | false;
  cfg.apAlways     = o["apAlways"] | true;

  cfg.cloudOn      = o["cloudOn"] | false;
  if (o.containsKey("cloudUrl")) copyStr(cfg.cloudUrl, sizeof cfg.cloudUrl, o["cloudUrl"] | "");
  if (o.containsKey("cloudToken"))
    copyStr(cfg.cloudToken, sizeof cfg.cloudToken, o["cloudToken"] | "");
  cfg.pollSec      = (uint16_t)constrain((int)(o["pollSec"] | 5), 3, 60);

  cfg.staticIp     = o["staticIp"] | false;
  if (o.containsKey("ip")) copyStr(cfg.ip, sizeof cfg.ip, o["ip"] | "");
  if (o.containsKey("gw")) copyStr(cfg.gw, sizeof cfg.gw, o["gw"] | "");
  if (o.containsKey("mask")) copyStr(cfg.mask, sizeof cfg.mask, o["mask"] | "255.255.255.0");
  if (o.containsKey("dns")) copyStr(cfg.dns, sizeof cfg.dns, o["dns"] | "223.5.5.5");

  // 省电设置
  cfg.powerSave    = o["powerSave"] | false;
  cfg.servoIdleSec = (uint16_t)constrain((int)(o["servoIdleSec"] | 5), 0, 3600);
  cfg.apAutoOffSec = (uint16_t)constrain((int)(o["apAutoOffSec"] | 60), 0, 3600);
  cfg.cpuMhz       = (uint16_t)(o["cpuMhz"] | 240);

  cfg.voiceOn      = o["voiceOn"] | false;
  cfg.voiceProto   = (uint8_t)constrain((int)(o["voiceProto"] | 0), 0, 1);
  if (o.containsKey("bemfaUid")) copyStr(cfg.bemfaUid, sizeof cfg.bemfaUid, o["bemfaUid"] | "");
  if (o.containsKey("bemfaTopic"))
    copyStr(cfg.bemfaTopic, sizeof cfg.bemfaTopic, o["bemfaTopic"] | "");
}

void taskToJson(JsonObject o, const Task &t) {
  o["enabled"] = t.enabled;
  o["hour"]    = t.hour;
  o["minute"]  = t.minute;
  o["days"]    = t.days;
  o["action"]  = t.action;
  o["once"]    = t.once;
  o["lastRun"] = t.lastRun;
}

void saveTasks() {
  DynamicJsonDocument d(4096);
  JsonArray                a = d.createNestedArray("tasks");
  for (int i = 0; i < taskCount; i++) {
    JsonObject o = a.createNestedObject();
    taskToJson(o, tasks[i]);
  }
  String s;
  serializeJson(d, s);
  prefs.putString("tasks", s);
}

void loadTasks() {
  taskCount     = 0;
  String s = loadStr("tasks");
  if (s.length() == 0) return;
  DynamicJsonDocument d(4096);
  if (deserializeJson(d, s)) {
    logAdd("任务解析失败, 已清空");
    return;
  }
  JsonArray a = d["tasks"].as<JsonArray>();
  for (JsonObject o : a) {
    if (taskCount >= MAX_TASKS) break;
    Task &t   = tasks[taskCount];
    memset(&t, 0, sizeof t);
    t.enabled = o["enabled"] | true;
    t.hour    = (uint8_t)constrain((int)(o["hour"] | 0), 0, 23);
    t.minute  = (uint8_t)constrain((int)(o["minute"] | 0), 0, 59);
    t.days    = (uint8_t)(o["days"] | 0x7F);
    t.action  = (uint8_t)constrain((int)(o["action"] | 0), 0, 2);
    t.once    = o["once"] | false;
    copyStr(t.lastRun, sizeof t.lastRun, o["lastRun"] | "");
    taskCount++;
  }
  logAdd("已加载 %d 个定时任务", taskCount);
}

// ---------------- 舵机控制 ----------------
// ---- 省电：舵机 PWM 空闲时断电 ----
// SG90 只要还在收 50Hz 脉冲就会持续出力保持角度，静止时白耗 50~150mA。
// 停掉 PWM 后舵机卸力（手指会松），静止位置不压按钮的话这是最划算的一笔。
volatile bool servoPwmOn   = true;
uint32_t      servoLastMove = 0;

void servoWrite(int angle);  // 前置声明

void servoPwmOff() {
  if (!servoPwmOn) return;
  ledcWrite(SERVO_CH, 0);  // 占空比 0 = 没有脉冲
  servoPwmOn = false;
  lastAngle  = -1;
  logAdd("舵机 PWM 已关闭（省电）");
}

void servoPwmResume() {
  if (servoPwmOn) return;
  servoPwmOn = true;
  lastAngle  = -1;
  servoWrite(cfg.restAngle);
  logAdd("舵机 PWM 已恢复");
}

void servoWrite(int angle) {
  int a = constrain(angle, 0, 180);
  servoLastMove = millis();
  if (!servoPwmOn) servoPwmOn = true;  // 重新开始输出
  if (a == lastAngle) return;
  lastAngle     = a;
  uint32_t us   = (uint32_t)(SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * a / 180);
  uint32_t duty = (uint32_t)((uint64_t)us * SERVO_MAXDUTY / 20000ULL);
  ledcWrite(SERVO_CH, duty);
}

void servoInit() {
  ledcSetup(SERVO_CH, SERVO_FREQ, SERVO_RES);
  ledcAttachPin(PIN_SERVO, SERVO_CH);
  lastAngle = -1;
  servoWrite(cfg.restAngle);
  delay(300);
}

bool servoBusy() { return phase == PH_MOVE || phase == PH_HOLD; }

void doPress(const char *why) {
  if (pendingPress < 5) pendingPress++;
  logAdd("按键触发(%s), 待执行 %d", why, pendingPress);
}

// 从当前角度平滑移动到 target，到位后进入 after 相位
void servoMoveTo(int target, ServoPhase after) {
  fromAngle      = (lastAngle < 0) ? (int)cfg.restAngle : lastAngle;
  toAngle        = constrain(target, 0, 180);
  phaseAfterMove = after;
  phase          = PH_MOVE;
  phaseAt        = millis();
}

// 手动摆到指定角度并保持（网页校准用，不会自动回位）
void servoManual(int angle) {
  pendingPress = 0;
  phase        = PH_MANUAL;
  servoWrite(angle);
  logAdd("手动角度 %d 度", constrain(angle, 0, 180));
}

void servoTick() {
  uint32_t now = millis();
  switch (phase) {
    case PH_IDLE:
    case PH_MANUAL:
      if (pendingPress > 0) {
        pendingPress--;
        servoMoveTo(cfg.pressAngle, PH_HOLD);
      }
      break;

    case PH_MOVE: {
      float p = (now - phaseAt) / (float)MOVE_MS;
      if (p > 1.0f) p = 1.0f;
      servoWrite((int)lroundf(fromAngle + (toAngle - (float)fromAngle) * p));
      if (p >= 1.0f) {
        phase   = phaseAfterMove;
        phaseAt = now;
      }
      break;
    }

    case PH_HOLD:
      if (now - phaseAt >= cfg.holdMs) servoMoveTo(cfg.restAngle, PH_IDLE);
      break;
  }
}

// action: 0=开机 1=关机 2=切换
void voicePublishState();  // 前置声明（定义在巴法云段落）

void applyAction(uint8_t action, const char *why) {
  if (action == 2) {
    doPress(why);
    cfg.powerOn = !cfg.powerOn;
    saveCfg();
    voicePublishState();
    return;
  }
  bool want = (action == 0);
  if (want == cfg.powerOn) {
    logAdd("%s: 已经是%s状态, 跳过", why, want ? "开机" : "关机");
    return;
  }
  doPress(why);
  cfg.powerOn = want;
  saveCfg();
  voicePublishState();
}

// ---------------- 手动按键 ----------------
void buttonTick() {
  static bool     last    = HIGH;
  static uint32_t lastAt  = 0;
  bool            cur     = digitalRead(PIN_BTN);
  uint32_t        now     = millis();
  if (cur != last && now - lastAt > 40) {
    lastAt = now;
    last   = cur;
    if (cur == LOW) {
      applyAction(2, "手动按键");
    }
  }
}

// ---------------- 定时任务 ----------------
char lastMinuteKey[24] = "";

void scheduleTick() {
  static uint32_t lastCheck = 0;
  uint32_t        now       = millis();
  if (now - lastCheck < 500) return;
  lastCheck = now;

  if (!cfg.schedEnabled) return;

  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (t.tm_year < 120) return;  // 时间还没同步

  char key[24];
  strftime(key, sizeof key, "%Y-%m-%d %H:%M", &t);
  if (strcmp(key, lastMinuteKey) == 0) return;
  copyStr(lastMinuteKey, sizeof lastMinuteKey, key);

  int wd = (t.tm_wday + 6) % 7;  // 0=周一
  for (int i = 0; i < taskCount; i++) {
    Task &k = tasks[i];
    if (!k.enabled) continue;
    if (k.hour != t.tm_hour || k.minute != t.tm_min) continue;
    if (!(k.days & (1 << wd))) continue;
    if (strcmp(k.lastRun, key) == 0) continue;

    copyStr(k.lastRun, sizeof k.lastRun, key);
    if (k.once) k.enabled = false;
    saveTasks();
    logAdd("执行任务 #%d (%02d:%02d)", i + 1, k.hour, k.minute);
    applyAction(k.action, "定时任务");
  }
}

// ---------------- WiFi ----------------
// 给 STA 打开 PMF(802.11w) 兼容。
// 有些路由器要求受保护管理帧，Arduino 默认只协商 WPA2，
// 表现就是 4WAY_HANDSHAKE_TIMEOUT（密码明明是对的，手机/电脑却都正常）。
void wifiEnablePmf() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return;
  if (conf.sta.pmf_cfg.capable) return;
  conf.sta.pmf_cfg.capable  = true;
  conf.sta.pmf_cfg.required = false;
  if (esp_wifi_set_config(WIFI_IF_STA, &conf) == ESP_OK) {
    logAdd("已开启 PMF(802.11w) 兼容");
  }
}

// 连接目标 WiFi。
// 注意：曾经试过"指定信道 + 指定 BSSID"去连，反而关联不上（原因不明），
// 所以这里退回最普通的 begin()，只用扫描结果打日志。
void wifiConnectBest() {
  int bestRssi = -1000;
  int n        = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != cfg.ssid) continue;
    if (WiFi.RSSI(i) > bestRssi) bestRssi = WiFi.RSSI(i);
  }
  WiFi.scanDelete();

  if (bestRssi > -1000) logAdd("连接 WiFi: %s (最强 AP %d dBm)", cfg.ssid, bestRssi);
  else logAdd("连接 WiFi: %s (未扫到, 直接尝试)", cfg.ssid);

  WiFi.begin(cfg.ssid, cfg.pass);
  wifiEnablePmf();
}

// 扫描并返回目标 SSID 最强 AP 的信道（0 表示没扫到）
int wifiBestChannel() {
  int n = WiFi.scanNetworks();
  int best = 0, bestRssi = -1000;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != cfg.ssid) continue;
    if (WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      best     = (int)WiFi.channel(i);
    }
  }
  WiFi.scanDelete();
  return best;
}

// 应用静态 IP（必须在 WiFi.begin 之前调用）
bool applyStaticIp() {
  if (!cfg.staticIp) return false;
  if (strlen(cfg.ip) == 0 || strlen(cfg.gw) == 0) {
    logAdd("静态 IP 未填完整, 用 DHCP");
    return false;
  }
  IPAddress ip, gw, mask, dns;
  if (!ip.fromString(cfg.ip) || !gw.fromString(cfg.gw) || !mask.fromString(cfg.mask)) {
    logAdd("静态 IP 格式错误, 用 DHCP");
    return false;
  }
  if (!dns.fromString(cfg.dns)) dns = gw;
  if (!WiFi.config(ip, gw, mask, dns)) {
    logAdd("静态 IP 设置失败, 用 DHCP");
    return false;
  }
  logAdd("静态 IP: %s / 网关 %s / 掩码 %s", cfg.ip, cfg.gw, cfg.mask);
  return true;
}

void wifiBegin() {
  WiFi.persistent(false);
  WiFi.setHostname(cfg.host);
  WiFi.mode(cfg.apAlways ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);       // 关省电：握手更稳，网页响应更快
  WiFi.setAutoReconnect(false);  // 自己控制重连节奏，避免抢射频把热点饿死
  if (cfg.powerSave) {
    WiFi.setSleep(true);  // 省电模式：开 modem sleep（省 40~60mA，代价是延迟高一点）
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // 更激进的省电：只在 DTIM 窗口醒来
    WiFi.setTxPower(WIFI_POWER_8_5dBm);  // 降发射功率（家里够用，省 10~30mA）
    logAdd("省电模式：modem sleep + 低发射功率 已开启");
  }

  // 热点固定在 ch1：之前试过"热点跟随路由器同信道"，结果热点反而扫不到
  //（STA 反复重连时射频被抢，beacon 发不出去），所以热点保持独立信道
  uint8_t apCh = 1;

  if (cfg.apAlways) {
    bool apOk = WiFi.softAP(AP_SSID, AP_PASS, apCh);
    logAdd("配网热点 %s %s (ch%d) -> http://%s", AP_SSID, apOk ? "已开启" : "开启失败",
           apCh, WiFi.softAPIP().toString().c_str());
  }
  if (strlen(cfg.ssid) > 0) {
    applyStaticIp();
    wifiConnectBest();
  } else {
    logAdd("未保存 WiFi, 请连热点后打开 http://192.168.4.1");
  }

  if (MDNS.begin(cfg.host)) {
    MDNS.addService("http", "tcp", 80);
    logAdd("mDNS 已启动: http://%s.local", cfg.host);
  }
}

void wifiTick() {
  static bool     wasConnected = false;
  static uint32_t lastTry      = 0;
  static uint32_t attemptAt    = 0;
  static uint8_t  attempt      = 0;
  static bool     trying       = false;

  if (strlen(cfg.ssid) == 0) return;

  bool now = (WiFi.status() == WL_CONNECTED);

  if (now) {
    trying = false;
    attempt = 0;
    if (!wasConnected) {
      wasConnected = true;
      logAdd("WiFi 已连接, IP=%s", WiFi.localIP().toString().c_str());
      startNtp();
    }
    return;
  }
  if (wasConnected) {
    wasConnected = false;
    logAdd("WiFi 断开, 准备重连");
  }

  // 关键：ESP32 自动重连会一直抢射频，导致热点 beacon 发不出去（手机就搜不到设备）。
  // 所以自己控制节奏：尝试 15 秒不成功就断开、安静等待，再按退避重试。
  if (trying) {
    if (millis() - attemptAt > 15000) {
      trying  = false;
      lastTry = millis();
      WiFi.disconnect(false, false);  // 停止底层反复重试
      logAdd("WiFi 连接失败, 第 %d 次，稍后重试", attempt);
    }
    return;
  }

  uint32_t waitMs = 30000UL << (attempt > 4 ? 4 : attempt);  // 30s,60s,120s,240s,480s
  if (waitMs > 600000UL) waitMs = 600000UL;
  if (millis() - lastTry > waitMs) {
    lastTry  = millis();
    attemptAt = millis();
    trying   = true;
    if (attempt < 6) attempt++;
    logAdd("尝试连接 WiFi (第 %d 次)", attempt);
    wifiConnectBest();
  }
}

// ---------------- 云端轮询（微信云开发 HTTP 访问服务 / 自建 HTTP 接口）----------------
// 协议：
//   设备 -> 云:  POST cloudUrl  {token, action:"sync", dev, status:{...}, tasklist:[...]}
//   云 -> 设备:  {ok:true, cmd:[{a:"press"}, {a:"angle", v:45}, {a:"tasks", list:[...]}]}
// 云端返回 cmd 后会自己清空队列；设备不做去重。
uint32_t cloudLast    = 0;
uint32_t cloudOkCount = 0;
uint32_t cloudErrCount = 0;
int      cloudLastCode = 0;

String buildCloudPayload() {
  DynamicJsonDocument d(4096);
  d["token"]  = cfg.cloudToken;
  d["action"] = "sync";
  d["dev"]    = cfg.host;

  JsonObject st = d.createNestedObject("status");
  st["power"] = cfg.powerOn;
  st["cur"]   = (lastAngle < 0) ? (int)cfg.restAngle : lastAngle;
  st["rest"]  = cfg.restAngle;
  st["press"] = cfg.pressAngle;
  st["hold"]  = cfg.holdMs;
  st["time"]  = (uint32_t)time(nullptr);
  st["rssi"]  = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  st["ip"]    = WiFi.localIP().toString();
  st["ver"]   = FW_VERSION;
  st["tasks"] = taskCount;
  st["sched"] = cfg.schedEnabled;

  JsonArray arr = d.createNestedArray("tasklist");
  for (int i = 0; i < taskCount; i++) {
    JsonObject o = arr.createNestedObject();
    taskToJson(o, tasks[i]);
  }
  String s;
  serializeJson(d, s);
  return s;
}

void cloudApplyTasks(JsonArray list) {
  int n = 0;
  for (JsonObject o : list) {
    if (n >= MAX_TASKS) break;
    Task &t   = tasks[n];
    memset(&t, 0, sizeof t);
    t.enabled = o["enabled"] | true;
    t.hour    = (uint8_t)constrain((int)(o["hour"] | 0), 0, 23);
    t.minute  = (uint8_t)constrain((int)(o["minute"] | 0), 0, 59);
    t.days    = (uint8_t)(o["days"] | 0x7F);
    t.action  = (uint8_t)constrain((int)(o["action"] | 0), 0, 2);
    t.once    = o["once"] | false;
    n++;
  }
  taskCount = n;
  saveTasks();
  logAdd("云端下发任务: %d 条", taskCount);
}

void cloudApply(JsonArray cmds) {
  for (JsonObject c : cmds) {
    const char *a = c["a"] | "";
    if (!strcmp(a, "press")) {
      applyAction(2, "云端");
    } else if (!strcmp(a, "on")) {
      applyAction(0, "云端");
    } else if (!strcmp(a, "off")) {
      applyAction(1, "云端");
    } else if (!strcmp(a, "angle")) {
      servoManual((int)(c["v"] | (int)cfg.restAngle));
    } else if (!strcmp(a, "tasks")) {
      cloudApplyTasks(c["list"].as<JsonArray>());
    } else if (!strcmp(a, "sched")) {
      cfg.schedEnabled = c["v"] | true;
      saveCfg();
    }
  }
}

void cloudTick() {
  if (!cfg.cloudOn || strlen(cfg.cloudUrl) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - cloudLast < (uint32_t)cfg.pollSec * 1000UL) return;
  cloudLast = millis();

  // http:// 用于本地 mock 调试，https:// 用于真实云端
  bool             isHttps = String(cfg.cloudUrl).startsWith("https://");
  WiFiClient       plain;
  WiFiClientSecure tls;
  tls.setInsecure();  // 不校验证书，省 RAM；公网 HTTPS 已够用

  HTTPClient http;
  http.setTimeout(5000);
  http.setReuse(false);

  bool began = isHttps ? http.begin(tls, cfg.cloudUrl) : http.begin(plain, cfg.cloudUrl);
  if (!began) {
    cloudErrCount++;
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(buildCloudPayload());
  cloudLastCode = code;

  if (code == 200) {
    String resp = http.getString();
    DynamicJsonDocument rd(4096);
    if (!deserializeJson(rd, resp)) {
      JsonArray cmds = rd["cmd"].as<JsonArray>();
      if (!cmds.isNull() && cmds.size() > 0) cloudApply(cmds);
      cloudOkCount++;
    } else {
      cloudErrCount++;
    }
  } else {
    cloudErrCount++;
  }
  http.end();
}

// ---------------- 语音音箱：巴法云 -> 米家 / 小爱同学 ----------------
// 两种协议都支持：
//   MQTT: bemfa.com:9501，ClientID = 私钥，订阅主题
//   TCP : bemfa.com:8344，发 cmd=1&uid=..&topic=..\r\n 订阅，60 秒内要发 heartbeat
// 米家「其他平台设备 → 巴法」只同步账号里的 TCP 或 MQTT 设备云，选哪种由主题类型决定，
// 所以固件这边做成可切换，避免平台限制。
WiFiClient   voiceNet;
PubSubClient mqtt(voiceNet);
WiFiClient   tcpVoice;
String       tcpLine;
uint32_t     voiceLastTry  = 0;
bool         voiceConnected = false;
uint32_t     voiceMsgCount = 0;
uint32_t     tcpLastPing   = 0;

void voicePublishState() {
  if (strlen(cfg.bemfaTopic) == 0) return;
  const char *v = cfg.powerOn ? "on" : "off";

  if (cfg.voiceProto == 1) {  // TCP
    if (!tcpVoice.connected()) return;
    tcpVoice.printf("cmd=2&uid=%s&topic=%s/up&msg=%s\r\n", cfg.bemfaUid, cfg.bemfaTopic, v);
    return;
  }
  if (!mqtt.connected()) return;
  char t[48];
  snprintf(t, sizeof t, "%s/up", cfg.bemfaTopic);  // /up 只更新云端数据
  mqtt.publish(t, v);
}

void voiceDispatch(const char *msg) {
  voiceMsgCount++;
  logAdd("收到语音指令: %s", msg);
  if (!strcmp(msg, "on")) applyAction(0, "小爱");
  else if (!strcmp(msg, "off")) applyAction(1, "小爱");
  else if (!strcmp(msg, "toggle")) applyAction(2, "小爱");
  // set:on / set:off —— 只同步状态，不按按钮（用于纠正逻辑状态漂移）
  else if (!strcmp(msg, "set:on")) {
    cfg.powerOn = true;
    saveCfg();
    logAdd("状态已同步为开机（未按按钮）");
  } else if (!strcmp(msg, "set:off")) {
    cfg.powerOn = false;
    saveCfg();
    logAdd("状态已同步为关机（未按按钮）");
  }
  voicePublishState();
}

void voiceCallback(char *topic, byte *payload, unsigned int len) {
  char msg[32];
  if (len > sizeof(msg) - 1) len = sizeof(msg) - 1;
  memcpy(msg, payload, len);
  msg[len] = 0;
  voiceDispatch(msg);
}

// ---- TCP 模式 ----
void tcpHandleLine(const String &line) {
  if (line.startsWith("cmd=1&res=1")) {
    voiceConnected = true;
    logAdd("巴法云(TCP)已订阅 %s（小爱可控制）", cfg.bemfaTopic);
    voicePublishState();
    return;
  }
  if (line.startsWith("cmd=0&res=1")) return;  // 心跳回应
  int i = line.indexOf("msg=");
  if (i >= 0) {
    String msg = line.substring(i + 4);
    voiceDispatch(msg.c_str());
  }
}

bool voiceConnectTcp() {
  if (!tcpVoice.connect("bemfa.com", 8344)) {
    logAdd("巴法云(TCP)连接失败");
    return false;
  }
  tcpVoice.printf("cmd=1&uid=%s&topic=%s\r\n", cfg.bemfaUid, cfg.bemfaTopic);
  tcpLastPing = millis();
  tcpLine     = "";
  logAdd("巴法云(TCP)已连接，订阅 %s ...", cfg.bemfaTopic);
  return true;
}

void voiceTickTcp() {
  if (!tcpVoice.connected()) {
    voiceConnected = false;
    if (millis() - voiceLastTry < 10000) return;
    voiceLastTry = millis();
    voiceConnectTcp();
    return;
  }
  // 心跳：60 秒内必须发，否则掉线
  if (millis() - tcpLastPing > 30000) {
    tcpVoice.print("ping\r\n");
    tcpLastPing = millis();
  }
  // 逐行解析
  while (tcpVoice.available()) {
    char c = (char)tcpVoice.read();
    if (c == '\n') {
      if (tcpLine.length()) tcpHandleLine(tcpLine);
      tcpLine = "";
    } else if (c != '\r' && tcpLine.length() < 200) {
      tcpLine += c;
    }
  }
}

// ---- MQTT 模式 ----
bool voiceConnectMqtt() {
  mqtt.setServer("bemfa.com", 9501);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(voiceCallback);

  logAdd("巴法云(MQTT)连接中...");
  if (mqtt.connect(cfg.bemfaUid)) {
    mqtt.subscribe(cfg.bemfaTopic);
    voiceConnected = true;
    logAdd("巴法云(MQTT)已连接，主题 %s（小爱可控制）", cfg.bemfaTopic);
    voicePublishState();
    return true;
  }
  logAdd("巴法云(MQTT)连接失败 rc=%d（私钥错误或网络问题）", mqtt.state());
  return false;
}

void voiceTick() {
  if (!cfg.voiceOn) return;
  if (strlen(cfg.bemfaUid) == 0 || strlen(cfg.bemfaTopic) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (cfg.voiceProto == 1) {
    voiceTickTcp();
    return;
  }

  if (!mqtt.connected()) {
    voiceConnected = false;
    if (millis() - voiceLastTry < 10000) return;
    voiceLastTry = millis();
    voiceConnectMqtt();
    return;
  }
  mqtt.loop();
}

// ---------------- HTTP 辅助 ----------------
void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}

void sendJson(int code, const String &body) {
  cors();
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

void sendJsonDoc(int code, JsonDocument &doc) {
  String s;
  serializeJson(doc, s);
  sendJson(code, s);
}

void sendOk(const char *msg) {
  sendJson(200, String("{\"ok\":true,\"msg\":\"") + msg + "\"}");
}

void sendErr(int code, const char *msg) {
  sendJson(code, String("{\"ok\":false,\"msg\":\"") + msg + "\"}");
}

// ---------------- 路由处理 ----------------
void handleRoot() {
  cors();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatus() {
  DynamicJsonDocument d(1024);
  struct tm                t;
  bool                     tv = getLocalTime(&t, 0);
  char                     ts[24] = "-";

  if (tv) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);

  d["ok"]      = true;
  d["fw"]      = FW_VERSION;
  d["name"]    = cfg.host;
  d["mode"]    = (WiFi.status() == WL_CONNECTED) ? "STA+AP" : (cfg.apAlways ? "AP" : "STA");
  d["ip"]      = WiFi.localIP().toString();
  d["apIp"]    = WiFi.softAPIP().toString();
  d["ssid"]    = WiFi.SSID();
  d["rssi"]    = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  d["uptime"]  = (uint32_t)(millis() / 1000);
  d["heap"]    = ESP.getFreeHeap();
  d["power"]   = cfg.powerOn;
  d["busy"]    = servoBusy();
  d["timeOk"]  = tv;
  d["time"]    = ts;
  d["epoch"]   = (uint32_t)time(nullptr);
  d["tzMin"]   = cfg.tzMin;
  d["tasks"]   = taskCount;
  d["sched"]   = cfg.schedEnabled;
  d["rest"]    = cfg.restAngle;
  d["press"]   = cfg.pressAngle;
  d["cur"]     = (lastAngle < 0) ? (int)cfg.restAngle : lastAngle;
  d["holdMs"]  = cfg.holdMs;
  d["powerSave"] = cfg.powerSave;
  d["servoOn"]   = servoPwmOn;
  d["voiceOn"]     = cfg.voiceOn;
  d["voiceOnline"] = (bool)(cfg.voiceProto == 1 ? tcpVoice.connected() : mqtt.connected());
  d["voiceTopic"]  = cfg.bemfaTopic;
  sendJsonDoc(200, d);
}

void handleConfigGet() {
  DynamicJsonDocument d(512);
  cfgToJson(d.to<JsonObject>());
  d["fw"]   = FW_VERSION;
  d["mode"] = (WiFi.status() == WL_CONNECTED) ? "STA+AP" : "AP";
  sendJsonDoc(200, d);
}

void handleConfigPost() {
  String body = server.arg("plain");
  DynamicJsonDocument d(768);
  if (deserializeJson(d, body)) {
    sendErr(400, "JSON 格式错误");
    return;
  }
  JsonObject o    = d.as<JsonObject>();
  bool       need = false;

  if (o.containsKey("ssid")) {
    copyStr(cfg.ssid, sizeof cfg.ssid, o["ssid"] | "");
    need = true;
  }
  if (o.containsKey("pass")) {
    const char *p = o["pass"] | "";
    if (strlen(p) > 0) {  // 空密码表示不修改
      copyStr(cfg.pass, sizeof cfg.pass, p);
      need = true;
    }
  }
  if (o.containsKey("host")) {
    const char *h = o["host"] | DEF_HOST;
    if (strlen(h) > 0) copyStr(cfg.host, sizeof cfg.host, h);
  }
  if (o.containsKey("schedEnabled")) cfg.schedEnabled = o["schedEnabled"] | true;
  if (o.containsKey("restAngle"))
    cfg.restAngle = (uint8_t)constrain((int)(o["restAngle"] | 25), 0, 180);
  if (o.containsKey("pressAngle"))
    cfg.pressAngle = (uint8_t)constrain((int)(o["pressAngle"] | 75), 0, 180);
  if (o.containsKey("holdMs"))
    cfg.holdMs = (uint16_t)constrain((int)(o["holdMs"] | 450), 50, 5000);
  if (o.containsKey("tzMin")) {
    cfg.tzMin = (int16_t)constrain((int)(o["tzMin"] | 480), -720, 840);
    applyTz();
  }
  if (o.containsKey("apAlways")) cfg.apAlways = o["apAlways"] | true;

  // ---- 云端轮询 ----
  if (o.containsKey("cloudOn")) cfg.cloudOn = o["cloudOn"] | false;
  if (o.containsKey("cloudUrl")) copyStr(cfg.cloudUrl, sizeof cfg.cloudUrl, o["cloudUrl"] | "");
  if (o.containsKey("cloudToken")) {
    const char *tk = o["cloudToken"] | "";
    if (strlen(tk) > 0) copyStr(cfg.cloudToken, sizeof cfg.cloudToken, tk);
  }
  if (o.containsKey("pollSec"))
    cfg.pollSec = (uint16_t)constrain((int)(o["pollSec"] | 5), 3, 60);

  // ---- 语音（巴法云）----
  if (o.containsKey("voiceOn")) cfg.voiceOn = o["voiceOn"] | false;
  if (o.containsKey("bemfaUid")) {
    const char *u = o["bemfaUid"] | "";
    if (strlen(u) > 0) copyStr(cfg.bemfaUid, sizeof cfg.bemfaUid, u);
  }
  if (o.containsKey("bemfaTopic")) {
    const char *t = o["bemfaTopic"] | "";
    if (strlen(t) > 0) copyStr(cfg.bemfaTopic, sizeof cfg.bemfaTopic, t);
  }
  voiceLastTry = 0;

  // ---- 静态 IP ----
  if (o.containsKey("staticIp")) cfg.staticIp = o["staticIp"] | false;
  if (o.containsKey("ip")) copyStr(cfg.ip, sizeof cfg.ip, o["ip"] | "");
  if (o.containsKey("gw")) copyStr(cfg.gw, sizeof cfg.gw, o["gw"] | "");
  if (o.containsKey("mask")) copyStr(cfg.mask, sizeof cfg.mask, o["mask"] | "255.255.255.0");
  if (o.containsKey("dns")) copyStr(cfg.dns, sizeof cfg.dns, o["dns"] | "223.5.5.5");

  cfg.powerSave    = o["powerSave"] | false;
  cfg.servoIdleSec = (uint16_t)constrain((int)(o["servoIdleSec"] | 5), 0, 3600);
  bool needRestartForIp = o.containsKey("staticIp") || o.containsKey("ip") || o.containsKey("gw");

  saveCfg();
  // 参数改了以后回到待机角度（手动校准中就先别打断）
  if (phase == PH_IDLE || phase == PH_MANUAL) {
    phase = PH_IDLE;
    servoWrite(cfg.restAngle);
  }

  bool restart = need || needRestartForIp;
  if (restart) {
    logAdd("网络配置已变更, 1 秒后重启");
    rebootAt = millis() + 1200;
  }
  sendOk(restart ? "已保存, 设备即将重启" : "已保存");
}

void handleTasksGet() {
  DynamicJsonDocument d(4096);
  JsonArray                a = d.createNestedArray("tasks");
  for (int i = 0; i < taskCount; i++) {
    JsonObject o = a.createNestedObject();
    taskToJson(o, tasks[i]);
  }
  d["ok"] = true;
  sendJsonDoc(200, d);
}

void handleTasksPost() {
  String body = server.arg("plain");
  DynamicJsonDocument d(4096);
  if (deserializeJson(d, body)) {
    sendErr(400, "JSON 格式错误");
    return;
  }
  JsonArray a = d["tasks"].as<JsonArray>();
  if (a.isNull()) {
    sendErr(400, "缺少 tasks 数组");
    return;
  }

  int n = 0;
  for (JsonObject o : a) {
    if (n >= MAX_TASKS) break;
    Task &t   = tasks[n];
    memset(&t, 0, sizeof t);
    t.enabled = o["enabled"] | true;
    t.hour    = (uint8_t)constrain((int)(o["hour"] | 0), 0, 23);
    t.minute  = (uint8_t)constrain((int)(o["minute"] | 0), 0, 59);
    t.days    = (uint8_t)(o["days"] | 0x7F);
    t.action  = (uint8_t)constrain((int)(o["action"] | 0), 0, 2);
    t.once    = o["once"] | false;
    n++;
  }
  taskCount = n;
  saveTasks();
  logAdd("定时任务已更新: %d 条", taskCount);
  sendJson(200, String("{\"ok\":true,\"msg\":\"已保存\",\"count\":") + taskCount + "}");
}

void handleAction() {
  String body = server.arg("plain");
  DynamicJsonDocument d(256);
  if (deserializeJson(d, body)) {
    sendErr(400, "JSON 格式错误");
    return;
  }
  const char *a = d["action"] | "";
  uint8_t     code;

  // 手动校准：{"action":"angle","angle":45} 直接把舵机摆到指定角度并保持
  if (!strcmp(a, "angle")) {
    int ang = (int)(d["angle"] | (int)cfg.restAngle);
    servoManual(ang);
    DynamicJsonDocument r(128);
    r["ok"]    = true;
    r["angle"] = constrain(ang, 0, 180);
    sendJsonDoc(200, r);
    return;
  }

  // 同步实际状态：只改记录，不按按钮。
  // 用途：有人手动按过机器 / 制冰机自己停机后，让固件的"逻辑状态"和现实对齐。
  if (!strcmp(a, "setstate")) {
    if (!d.containsKey("on")) {
      sendErr(400, "需要 on:true/false");
      return;
    }
    cfg.powerOn = d["on"] | false;
    saveCfg();
    voicePublishState();
    logAdd("同步电源状态为: %s（未按按钮）", cfg.powerOn ? "开机" : "关机");
    DynamicJsonDocument r(128);
    r["ok"]    = true;
    r["power"] = cfg.powerOn;
    sendJsonDoc(200, r);
    return;
  }

  if (!strcmp(a, "press")) code = 2;
  else if (!strcmp(a, "on")) code = 0;
  else if (!strcmp(a, "off")) code = 1;
  else {
    sendErr(400, "action 只能是 press/on/off/angle/setstate");
    return;
  }
  applyAction(code, "网页操作");
  DynamicJsonDocument r(128);
  r["ok"]    = true;
  r["power"] = cfg.powerOn;
  sendJsonDoc(200, r);
}

void handleTimeSet() {
  DynamicJsonDocument d(256);
  if (deserializeJson(d, server.arg("plain"))) {
    sendErr(400, "JSON 格式错误");
    return;
  }
  uint32_t epoch = (uint32_t)(d["epoch"] | 0UL);
  if (epoch < 1600000000UL) {
    sendErr(400, "epoch 无效");
    return;
  }
  if (d.containsKey("tzMin")) {
    cfg.tzMin = (int16_t)constrain((int)(d["tzMin"] | 480), -720, 840);
    saveCfg();
  }
  struct timeval tv;
  tv.tv_sec  = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  applyTz();
  logAdd("手动对时: %u", epoch);
  sendOk("时间已设置");
}

void handleWifiSet() {
  DynamicJsonDocument d(384);
  if (deserializeJson(d, server.arg("plain"))) {
    sendErr(400, "JSON 格式错误");
    return;
  }
  const char *ssid = d["ssid"] | "";
  const char *pass = d["pass"] | "";
  if (strlen(ssid) == 0) {
    sendErr(400, "ssid 不能为空");
    return;
  }
  copyStr(cfg.ssid, sizeof cfg.ssid, ssid);
  copyStr(cfg.pass, sizeof cfg.pass, pass);
  saveCfg();
  logAdd("WiFi 已更新: %s, 即将重启", ssid);
  rebootAt = millis() + 1000;
  sendOk("已保存, 设备正在重启");
}

void handleScan() {
  int n = WiFi.scanNetworks();
  DynamicJsonDocument d(2048);
  JsonArray a = d.createNestedArray("networks");
  for (int i = 0; i < n && i < 20; i++) {
    JsonObject o = a.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  d["ok"] = true;
  sendJsonDoc(200, d);
  WiFi.scanDelete();
}

void handleLogs() {
  DynamicJsonDocument d(3072);
  JsonArray a = d.createNestedArray("logs");
  int start = (logHead - logCount + LOG_N) % LOG_N;
  for (int i = 0; i < logCount; i++) {
    a.add(logBuf[(start + i) % LOG_N]);
  }
  d["ok"] = true;
  sendJsonDoc(200, d);
}

void handleReboot() {
  sendOk("正在重启");
  rebootAt = millis() + 600;
}

void handleReset() {
  prefs.clear();
  sendOk("已恢复出厂设置, 即将重启");
  rebootAt = millis() + 900;
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    cors();
    server.send(204);
    return;
  }
  if (server.uri().startsWith("/api/v1/")) {
    sendErr(501, "预留接口, 待后续版本实现");
    return;
  }
  sendErr(404, "not found");
}

void routes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/tasks", HTTP_GET, handleTasksGet);
  server.on("/api/tasks", HTTP_POST, handleTasksPost);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/time", HTTP_POST, handleTimeSet);
  server.on("/api/wifi", HTTP_POST, handleWifiSet);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
}

// ---------------- USB 串口命令行（不用 WiFi 也能全功能配置） ----------------
// 电脑上打开串口 (115200)，输入 help 查看命令
void cliHelp() {
  Serial.println(F(""));
  Serial.println(F("===== 制冰机手指机器人 串口命令 ====="));
  Serial.println(F("status                  查看状态"));
  Serial.println(F("press | on | off        按一下 / 设为开机 / 设为关机"));
  Serial.println(F("angle <0-180>           舵机直接转到某角度（校准）"));
  Serial.println(F("rest <0-180>            设置抬起角度"));
  Serial.println(F("down <0-180>            设置压下角度"));
  Serial.println(F("hold <50-5000>          设置按住毫秒"));
  Serial.println(F("task list               列出定时任务"));
  Serial.println(F("task add HH:MM on|off|toggle [1234567] [once]"));
  Serial.println(F("        星期用 1-7 表示周一到周日，不填=每天"));
  Serial.println(F("task del <n>            删除第 n 条"));
  Serial.println(F("task clear              清空任务"));
  Serial.println(F("sched on|off            定时总开关"));
  Serial.println(F("ap on|off               开/关设备热点（排查联网问题时用）"));
  Serial.println(F("cloud                   查看云轮询状态"));
  Serial.println(F("cloud on|off            开关云轮询"));
  Serial.println(F("cloud <url> <token>     设置云端地址与令牌"));
  Serial.println(F("cloud poll <秒>         设置轮询间隔(3~60)"));
  Serial.println(F("ip                      查看 IP 配置"));
  Serial.println(F("ip static <IP> <掩码> <网关> [DNS]  固定 IP（重启生效）"));
  Serial.println(F("ip dhcp                 改回自动获取"));
  Serial.println(F("power on|off            省电模式（WiFi 休眠 + 舵机空闲断电 + 关热点）"));
  Serial.println(F("cpu 80|160|240          设置 CPU 频率（省电可降到 80）"));
  Serial.println(F("voice <私钥> <主题>      接入巴法云，主题后三位 006=开关（例 bingji006）"));
  Serial.println(F("voice on|off|status     语音控制开关/状态"));
  Serial.println(F("state on|off|toggle     同步实际开关状态（只改记录不按按钮）"));
  Serial.println(F("time                    查看时间"));
  Serial.println(F("time 2026-10-18 08:00:00  手动设置时间"));
  Serial.println(F("wifi <ssid> <password>   设置 WiFi（重启生效）"));
  Serial.println(F("scan                    扫描附近 WiFi"));
  Serial.println(F("logs                    查看最近日志"));
  Serial.println(F("reset | reboot          恢复出厂 / 重启"));
  Serial.println(F("====================================="));
  Serial.println(F(""));
}

void cliStatus() {
  struct tm t;
  char      ts[24] = "-";
  if (getLocalTime(&t, 0)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);
  Serial.printf("固件      : %s\n", FW_VERSION);
  Serial.printf("网络      : %s  IP=%s  AP=%s\n",
                WiFi.status() == WL_CONNECTED ? "已连接" : "未连接",
                WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str());
  Serial.printf("WiFi SSID : %s\n", strlen(cfg.ssid) ? cfg.ssid : "(未设置)");
  Serial.printf("时间      : %s\n", ts);
  Serial.printf("电源状态  : %s\n", cfg.powerOn ? "开机" : "关机");
  Serial.printf("舵机      : 抬起 %d° / 压下 %d° / 按住 %dms / %s\n", cfg.restAngle,
                cfg.pressAngle, cfg.holdMs, servoBusy() ? "动作中" : "待机");
  Serial.printf("定时      : %s，共 %d 条任务\n", cfg.schedEnabled ? "已启用" : "已停用", taskCount);
  Serial.printf("内存      : %u 字节\n", (unsigned)ESP.getFreeHeap());
}

void cliTaskList() {
  if (taskCount == 0) {
    Serial.println(F("(没有任务)"));
    return;
  }
  const char *actName[] = {"开机", "关机", "切换"};
  for (int i = 0; i < taskCount; i++) {
    Task &k = tasks[i];
    // 每个中文星期名 3 字节 UTF-8，7 天最多 21 字节，留足空间
    char        days[32] = "";
    const char *dow[]    = {"一", "二", "三", "四", "五", "六", "日"};
    for (int d = 0; d < 7; d++)
      if (k.days & (1 << d)) strcat(days, dow[d]);
    Serial.printf("#%d %s %02d:%02d %s 周[%s]%s%s\n", i + 1, k.enabled ? "[开]" : "[关]",
                  k.hour, k.minute, actName[k.action], days[0] ? days : "无",
                  k.once ? " 单次" : "", k.lastRun[0] ? " " : "");
  }
}

// 返回 true 表示命令已处理
bool cliRun(char *line) {
  // 去掉首尾空白
  while (*line == ' ' || *line == '\t') line++;
  int len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\n'))
    line[--len] = 0;
  if (len == 0) return true;

  char cmd[24] = "";
  sscanf(line, "%23s", cmd);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { cliHelp(); return true; }
  if (!strcmp(cmd, "status")) { cliStatus(); return true; }
  if (!strcmp(cmd, "press")) { applyAction(2, "串口"); return true; }
  if (!strcmp(cmd, "on")) { applyAction(0, "串口"); return true; }
  if (!strcmp(cmd, "off")) { applyAction(1, "串口"); return true; }
  if (!strcmp(cmd, "reboot")) { Serial.println(F("重启中...")); rebootAt = millis() + 300; return true; }
  if (!strcmp(cmd, "reset")) { prefs.clear(); Serial.println(F("已清空配置，重启中...")); rebootAt = millis() + 500; return true; }

  if (!strcmp(cmd, "angle")) {
    int v = 0;
    if (sscanf(line, "angle %d", &v) == 1) { servoManual(v); Serial.printf("已转到 %d°\n", constrain(v, 0, 180)); }
    return true;
  }
  if (!strcmp(cmd, "rest") || !strcmp(cmd, "down") || !strcmp(cmd, "hold")) {
    int v = 0;
    if (sscanf(line, "%*s %d", &v) != 1) return true;
    if (!strcmp(cmd, "rest")) cfg.restAngle = (uint8_t)constrain(v, 0, 180);
    else if (!strcmp(cmd, "down")) cfg.pressAngle = (uint8_t)constrain(v, 0, 180);
    else cfg.holdMs = (uint16_t)constrain(v, 50, 5000);
    saveCfg();
    if (phase == PH_IDLE || phase == PH_MANUAL) { phase = PH_IDLE; servoWrite(cfg.restAngle); }
    Serial.printf("已保存: 抬起 %d° 压下 %d° 按住 %dms\n", cfg.restAngle, cfg.pressAngle, cfg.holdMs);
    return true;
  }

  if (!strcmp(cmd, "sched")) {
    char v[8] = "";
    sscanf(line, "sched %7s", v);
    if (!strcmp(v, "on")) cfg.schedEnabled = true;
    else if (!strcmp(v, "off")) cfg.schedEnabled = false;
    saveCfg();
    Serial.printf("定时: %s\n", cfg.schedEnabled ? "已启用" : "已停用");
    return true;
  }

  if (!strcmp(cmd, "power")) {
    char sub[12] = "";
    sscanf(line, "power %11s", sub);
    if (!strcmp(sub, "on")) {
      cfg.powerSave = true;
      saveCfg();
      WiFi.setSleep(true);
      esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
      WiFi.setTxPower(WIFI_POWER_8_5dBm);
      Serial.println(F("省电模式已开启：WiFi 深度休眠 + 低发射功率 + 舵机空闲断电 + 联网 60 秒后关热点"));
      Serial.println(F("（舵机静止会卸力，如果手指会掉下来压到按钮，把 servoIdleSec 设 0）"));
      return true;
    }
    if (!strcmp(sub, "off")) {
      cfg.powerSave = false;
      saveCfg();
      WiFi.setSleep(false);
      esp_wifi_set_ps(WIFI_PS_NONE);
      WiFi.setTxPower(WIFI_POWER_19_5dBm);
      servoPwmResume();
      Serial.println(F("省电模式已关闭"));
      return true;
    }
    Serial.printf("省电模式: %s\n", cfg.powerSave ? "开启" : "关闭");
    Serial.printf("舵机 PWM : %s（空闲 %d 秒后断电）\n", servoPwmOn ? "输出中" : "已关闭",
                  cfg.servoIdleSec);
    Serial.printf("WiFi     : %s\n", cfg.powerSave ? "MAX_MODEM 深度休眠" : "不休眠（响应快）");
    Serial.printf("发射功率 : %.1f dBm\n", WiFi.getTxPower() / 4.0);
    Serial.printf("CPU      : %d MHz\n", getCpuFrequencyMhz());
    Serial.printf("热点     : %s\n", cfg.apAlways ? "开" : "关闭");
    Serial.printf("热点关闭 : 联网后 %d 秒（0=永不）\n", cfg.apAutoOffSec);
    return true;
  }

  if (!strcmp(cmd, "state")) {
    char sub[12] = "";
    sscanf(line, "state %11s", sub);
    if (!strcmp(sub, "on")) {
      cfg.powerOn = true;
      saveCfg();
      voicePublishState();
      Serial.println(F("已同步为「开机」状态（没有按按钮）"));
      return true;
    }
    if (!strcmp(sub, "off")) {
      cfg.powerOn = false;
      saveCfg();
      voicePublishState();
      Serial.println(F("已同步为「关机」状态（没有按按钮）"));
      return true;
    }
    if (!strcmp(sub, "toggle")) {
      cfg.powerOn = !cfg.powerOn;
      saveCfg();
      voicePublishState();
      Serial.printf("状态已取反为: %s\n", cfg.powerOn ? "开机" : "关机");
      return true;
    }
    Serial.printf("当前逻辑状态: %s\n", cfg.powerOn ? "开机" : "关机");
    Serial.println(F("用法: state on|off|toggle   （只改记录，不按按钮，用来纠正开关反了）"));
    return true;
  }

  if (!strcmp(cmd, "voice")) {
    char sub[48] = "";
    sscanf(line, "voice %47s", sub);

    if (!strcmp(sub, "on")) {
      cfg.voiceOn = true;
      saveCfg();
      voiceLastTry = 0;
      Serial.println(F("语音控制已开启（巴法云）"));
      return true;
    }
    if (!strcmp(sub, "off")) {
      cfg.voiceOn = false;
      saveCfg();
      if (mqtt.connected()) mqtt.disconnect();
      if (tcpVoice.connected()) tcpVoice.stop();
      voiceConnected = false;
      Serial.println(F("语音控制已关闭"));
      return true;
    }
    if (sub[0] == 0 || !strcmp(sub, "status")) {
      bool online = (cfg.voiceProto == 1) ? tcpVoice.connected() : mqtt.connected();
      Serial.printf("语音控制: %s\n", cfg.voiceOn ? "开启" : "关闭");
      Serial.printf("协议    : %s\n", cfg.voiceProto == 1 ? "TCP (bemfa.com:8344)" : "MQTT (bemfa.com:9501)");
      Serial.printf("私钥    : %s\n", strlen(cfg.bemfaUid) ? "已设置" : "(未设置)");
      Serial.printf("主题    : %s\n", strlen(cfg.bemfaTopic) ? cfg.bemfaTopic : "(未设置)");
      Serial.printf("连接    : %s\n", online ? "已连接" : "未连接");
      Serial.printf("收到指令: %u 条\n", voiceMsgCount);
      return true;
    }

    // voice tcp|mqtt <私钥> <主题>
    char proto[8] = "", uid[40] = "", topic[33] = "";
    if (sscanf(line, "voice %7s %39s %32s", proto, uid, topic) == 3) {
      if (strcmp(proto, "tcp") && strcmp(proto, "mqtt")) {
        Serial.println(F("用法: voice tcp|mqtt <私钥> <主题>"));
        return true;
      }
      cfg.voiceProto = !strcmp(proto, "tcp") ? 1 : 0;
      copyStr(cfg.bemfaUid, sizeof cfg.bemfaUid, uid);
      copyStr(cfg.bemfaTopic, sizeof cfg.bemfaTopic, topic);
      cfg.voiceOn  = true;
      voiceLastTry = 0;
      if (mqtt.connected()) mqtt.disconnect();
      if (tcpVoice.connected()) tcpVoice.stop();
      saveCfg();
      Serial.printf("已配置巴法云: 协议=%s 主题=%s\n", cfg.voiceProto == 1 ? "TCP" : "MQTT", topic);
      Serial.println(F("米家：我的 → 其他平台设备 → 添加 → 巴法 → 登录同一个账号"));
      return true;
    }
    Serial.println(F("用法: voice tcp|mqtt <私钥> <主题>   或 voice on|off|status"));
    return true;
  }

  if (!strcmp(cmd, "cpu")) {
    char sub[8] = "";
    sscanf(line, "cpu %7s", sub);
    int m = atoi(sub);
    if (m == 80 || m == 160 || m == 240) {
      setCpuFrequencyMhz(m);
      cfg.cpuMhz = (uint16_t)m;
      saveCfg();
      Serial.printf("CPU 已切到 %d MHz（当前 %d MHz），已保存\n", m, getCpuFrequencyMhz());
      Serial.println(F("提示：改频率后串口可能有短暂乱码，属正常"));
    } else {
      Serial.printf("CPU 当前 %d MHz\n用法: cpu 80 | 160 | 240\n", getCpuFrequencyMhz());
    }
    return true;
  }

  if (!strcmp(cmd, "ip")) {
    char sub[12] = "";
    sscanf(line, "ip %11s", sub);

    if (!strcmp(sub, "dhcp")) {
      cfg.staticIp = false;
      saveCfg();
      Serial.println(F("已改回 DHCP，重启后生效"));
      return true;
    }
    if (!strcmp(sub, "static")) {
      char a[16] = "", m[16] = "", g[16] = "", d[16] = "";
      if (sscanf(line, "ip static %15s %15s %15s %15s", a, m, g, d) >= 3) {
        copyStr(cfg.ip, sizeof cfg.ip, a);
        copyStr(cfg.mask, sizeof cfg.mask, m);
        copyStr(cfg.gw, sizeof cfg.gw, g);
        if (d[0]) copyStr(cfg.dns, sizeof cfg.dns, d);
        cfg.staticIp = true;
        saveCfg();
        Serial.printf("静态 IP 已保存: %s / 掩码 %s / 网关 %s，重启后生效\n", a, m, g);
      } else {
        Serial.println(F("用法: ip static 192.168.1.50 255.255.255.0 192.168.1.1 [dns]"));
      }
      return true;
    }
    Serial.printf("IP 模式: %s\n", cfg.staticIp ? "静态" : "DHCP");
    Serial.printf("当前 IP : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("实际网关: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("实际掩码: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("实际 DNS: %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("配置的  : ip=%s gw=%s mask=%s dns=%s\n", cfg.ip, cfg.gw, cfg.mask, cfg.dns);
    Serial.println(F("固定 IP 建议：用上面这些值，把 IP 换成同网段没人用的地址"));
    return true;
  }

  if (!strcmp(cmd, "cloud")) {
    char sub[160] = "";
    sscanf(line, "cloud %159s", sub);

    if (sub[0] == 0 || !strcmp(sub, "status")) {
      Serial.printf("云轮询: %s  间隔 %ds  url=%s\n", cfg.cloudOn ? "开启" : "关闭", cfg.pollSec,
                    strlen(cfg.cloudUrl) ? cfg.cloudUrl : "(未设置)");
      Serial.printf("成功 %u 次 / 失败 %u 次, 上次 HTTP=%d\n", cloudOkCount, cloudErrCount,
                    cloudLastCode);
      return true;
    }
    if (!strcmp(sub, "on")) {
      cfg.cloudOn = true;
      saveCfg();
      Serial.println(F("云轮询已开启"));
      return true;
    }
    if (!strcmp(sub, "off")) {
      cfg.cloudOn = false;
      saveCfg();
      Serial.println(F("云轮询已关闭"));
      return true;
    }
    if (!strcmp(sub, "poll")) {
      int v = 5;
      sscanf(line, "cloud poll %d", &v);
      cfg.pollSec = (uint16_t)constrain(v, 3, 60);
      saveCfg();
      Serial.printf("轮询间隔: %d 秒\n", cfg.pollSec);
      return true;
    }
    // cloud <url> <token>
    char url[160] = "", tok[48] = "";
    if (sscanf(line, "cloud %159s %47s", url, tok) >= 1) {
      copyStr(cfg.cloudUrl, sizeof cfg.cloudUrl, url);
      if (tok[0]) copyStr(cfg.cloudToken, sizeof cfg.cloudToken, tok);
      cfg.cloudOn = true;
      saveCfg();
      Serial.printf("云端地址: %s\n轮询: 已开启, 每 %d 秒\n", cfg.cloudUrl, cfg.pollSec);
      return true;
    }
    return true;
  }

  if (!strcmp(cmd, "ap")) {
    char v[8] = "";
    sscanf(line, "ap %7s", v);
    if (!strcmp(v, "off")) {
      WiFi.softAPdisconnect(true);
      cfg.apAlways = false;
      saveCfg();
      Serial.println(F("热点已关闭（重启后仍关闭，用 ap on 恢复）"));
    } else if (!strcmp(v, "on")) {
      WiFi.softAP(AP_SSID, AP_PASS);
      cfg.apAlways = true;
      saveCfg();
      Serial.printf("热点已开启: http://%s\n", WiFi.softAPIP().toString().c_str());
    } else {
      Serial.printf("热点: %s\n", cfg.apAlways ? "开启" : "关闭");
    }
    return true;
  }

  if (!strcmp(cmd, "task")) {
    char sub[12] = "";
    sscanf(line, "task %11s", sub);

    if (!strcmp(sub, "list") || sub[0] == 0) { cliTaskList(); return true; }

    if (!strcmp(sub, "clear")) {
      taskCount = 0;
      saveTasks();
      Serial.println(F("已清空任务"));
      return true;
    }
    if (!strcmp(sub, "del")) {
      int n = 0;
      sscanf(line, "task del %d", &n);
      if (n >= 1 && n <= taskCount) {
        for (int i = n - 1; i < taskCount - 1; i++) tasks[i] = tasks[i + 1];
        taskCount--;
        saveTasks();
        Serial.printf("已删除 #%d\n", n);
      } else {
        Serial.println(F("序号错误"));
      }
      return true;
    }
    if (!strcmp(sub, "add")) {
      int  hh = 0, mm = 0;
      char act[16] = "";
      if (sscanf(line, "task add %d:%d %15s", &hh, &mm, act) < 3) {
        Serial.println(F("用法: task add 08:00 on|off|toggle [1234567] [once]"));
        return true;
      }
      if (taskCount >= MAX_TASKS) { Serial.println(F("任务已满(16)")); return true; }

      Task &k   = tasks[taskCount];
      memset(&k, 0, sizeof k);
      k.enabled = true;
      k.hour    = (uint8_t)constrain(hh, 0, 23);
      k.minute  = (uint8_t)constrain(mm, 0, 59);
      k.action  = !strcmp(act, "on") ? 0 : (!strcmp(act, "off") ? 1 : 2);
      k.days    = 0x7F;

      // 可选：星期数字 / once
      bool        dayGiven = false;
      const char *rest     = strstr(line, act);
      if (rest) rest += strlen(act);
      while (rest && *rest) {
        while (*rest == ' ') rest++;
        if (!*rest) break;
        if (!strncmp(rest, "once", 4)) { k.once = true; rest += 4; continue; }
        if (*rest >= '1' && *rest <= '7') {
          if (!dayGiven) { k.days = 0; dayGiven = true; }
          k.days |= (1 << (*rest - '1'));
          rest++;
          continue;
        }
        break;
      }
      taskCount++;
      saveTasks();
      Serial.printf("已添加任务 #%d\n", taskCount);
      cliTaskList();
      return true;
    }
    cliTaskList();
    return true;
  }

  if (!strcmp(cmd, "time")) {
    int Y, M, D, h, m, s;
    if (sscanf(line, "time %d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
      struct tm tmv = {};
      tmv.tm_year = Y - 1900;
      tmv.tm_mon  = M - 1;
      tmv.tm_mday = D;
      tmv.tm_hour = h;
      tmv.tm_min  = m;
      tmv.tm_sec  = s;
      time_t e    = mktime(&tmv);
      struct timeval tv;
      tv.tv_sec  = e;
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      applyTz();
      Serial.println(F("时间已设置"));
    } else {
      struct tm t;
      if (getLocalTime(&t, 0)) {
        char ts[24];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &t);
        Serial.printf("当前时间: %s\n", ts);
      } else {
        Serial.println(F("时间未同步（可用 time 2026-10-18 08:00:00 手动设置）"));
      }
    }
    return true;
  }

  if (!strcmp(cmd, "wifi")) {
    char ssid[33] = "", pass[65] = "";
    if (sscanf(line, "wifi %32s %64s", ssid, pass) < 1) {
      Serial.println(F("用法: wifi <ssid> <password>"));
      return true;
    }
    copyStr(cfg.ssid, sizeof cfg.ssid, ssid);
    copyStr(cfg.pass, sizeof cfg.pass, pass);
    saveCfg();
    Serial.printf("已保存 WiFi: %s，2 秒后重启\n", ssid);
    rebootAt = millis() + 2000;
    return true;
  }

  if (!strcmp(cmd, "scan")) {
    Serial.println(F("扫描中..."));
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 15; i++)
      Serial.printf("  %-24s %d dBm  ch%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    WiFi.scanDelete();
    return true;
  }

  if (!strcmp(cmd, "logs")) {
    int start = (logHead - logCount + LOG_N) % LOG_N;
    for (int i = 0; i < logCount; i++) Serial.println(logBuf[(start + i) % LOG_N]);
    return true;
  }

  Serial.printf("未知命令: %s（输入 help）\n", cmd);
  return true;
}

void cliTick() {
  static char   buf[96];
  static int    idx = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[idx] = 0;
      if (idx > 0) cliRun(buf);
      idx = 0;
      continue;
    }
    if (idx < (int)sizeof(buf) - 1) buf[idx++] = c;
  }
}

// ---------------- 省电 ----------------
void powerTick() {
  if (!cfg.powerSave) return;

  // 1) 舵机静止一段时间后停 PWM
  if (cfg.servoIdleSec > 0 && servoPwmOn && phase == PH_IDLE && pendingPress == 0) {
    if (millis() - servoLastMove > (uint32_t)cfg.servoIdleSec * 1000UL) {
      servoPwmOff();
    }
  }

  // 2) 联网稳定 5 分钟后关掉热点（省 20~30mA）；断网时自动恢复热点
  static uint32_t connectedAt    = 0;
  static bool     apAutoOffDone  = false;
  static bool     wasConn        = false;
  bool            conn           = (WiFi.status() == WL_CONNECTED);

  if (conn && !wasConn) {
    wasConn     = true;
    connectedAt = millis();
  }
  if (!conn && wasConn) {
    wasConn = false;
    if (apAutoOffDone) {
      WiFi.softAP(AP_SSID, AP_PASS);
      apAutoOffDone = false;
      logAdd("断网了，热点已重新开启（兜底）");
    }
  }
  if (conn && cfg.apAlways && !apAutoOffDone && cfg.apAutoOffSec > 0 &&
      millis() - connectedAt > (uint32_t)cfg.apAutoOffSec * 1000UL) {
    WiFi.softAPdisconnect(true);
    apAutoOffDone = true;
    logAdd("联网稳定 %d 秒，热点已关闭（省电）；需要时用 ap on 打开", cfg.apAutoOffSec);
  }
}

// ---------------- 主流程 ----------------
#ifdef SELFTEST
// 自检模式：不开 WiFi，直接对本机 80 端口发 HTTP 请求，
// 把 Web 服务 / JSON / 任务 / 舵机 全链路跑一遍（不需要路由器，也不需要手机）
bool httpReq(const char *method, const char *path, const char *body, String &resp) {
  WiFiClient c;
  if (!c.connect(WiFi.softAPIP(), 80)) return false;

  String req = String(method) + " " + path + " HTTP/1.1\r\n";
  req += "Host: 192.168.4.1\r\nConnection: close\r\n";
  if (body) {
    req += "Content-Type: application/json\r\nContent-Length: ";
    req += (unsigned)strlen(body);
    req += "\r\n";
  }
  req += "\r\n";
  if (body) req += body;
  c.print(req);

  uint32_t to = millis() + 3000;
  while (millis() < to) {
    server.handleClient();
    while (c.available()) resp += (char)c.read();
    if (!c.connected() && !c.available()) break;
    delay(1);
  }
  c.stop();
  return resp.length() > 0;
}

void selfTest() {
  logAdd("========== 自检开始 ==========");
  struct Case {
    const char *m;
    const char *p;
    const char *b;
  };
  const Case cases[] = {
      {"GET", "/api/status", nullptr},
      {"GET", "/api/config", nullptr},
      {"POST", "/api/config", "{\"holdMs\":500,\"restAngle\":30}"},
      {"POST", "/api/tasks",
       "{\"tasks\":[{\"enabled\":true,\"hour\":7,\"minute\":30,\"days\":127,"
       "\"action\":0,\"once\":false}]}"},
      {"GET", "/api/tasks", nullptr},
      {"POST", "/api/action", "{\"action\":\"press\"}"},
      {"POST", "/api/action", "{\"action\":\"angle\",\"angle\":120}"},
      {"GET", "/api/logs", nullptr},
      {"GET", "/", nullptr},
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    String r;
    bool   ok  = httpReq(cases[i].m, cases[i].p, cases[i].b, r);
    int    nl  = r.indexOf("\r\n");
    String head = (nl > 0) ? r.substring(0, nl) : r.substring(0, 40);
    logAdd("%-4s %-13s -> %s | %u bytes", cases[i].m, cases[i].p,
           ok ? head.c_str() : "连接失败", (unsigned)r.length());
    delay(150);
  }
  logAdd("任务数=%d 电源=%d 舵机忙=%d", taskCount, (int)cfg.powerOn, (int)servoBusy());

  // ---- 定时任务链路验证：把时间设到整分钟前 10 秒，建一条"下一分钟"的任务 ----
  const time_t base = 1760782850;  // 该时刻秒数 = 50
  struct timeval tv;
  tv.tv_sec  = base;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  applyTz();

  struct tm nt;
  time_t    next = base + 10;
  localtime_r(&next, &nt);

  char body[192];
  snprintf(body, sizeof body,
           "{\"tasks\":[{\"enabled\":true,\"hour\":%d,\"minute\":%d,"
           "\"days\":127,\"action\":2,\"once\":false}]}",
           nt.tm_hour, nt.tm_min);
  String r;
  httpReq("POST", "/api/tasks", body, r);
  logAdd("已建定时任务 %02d:%02d, 等待自动触发...", nt.tm_hour, nt.tm_min);

  bool     before = cfg.powerOn;
  uint32_t until  = millis() + 16000;
  while (millis() < until) {
    server.handleClient();
    servoTick();
    scheduleTick();
    delay(5);
  }
  logAdd("等待结束: 电源 %d -> %d (toggle 应该翻转)", (int)before, (int)cfg.powerOn);

  logAdd("========== 自检结束 ==========");
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Ice Finger Bot " FW_VERSION " ===");

  prefs.begin("icebot", false);
  loadCfg();
  loadTasks();
  applyTz();
  if (cfg.cpuMhz == 80 || cfg.cpuMhz == 160) {
    setCpuFrequencyMhz(cfg.cpuMhz);
    Serial.printf("[省电] CPU 已设为 %d MHz\n", getCpuFrequencyMhz());
  }

  pinMode(PIN_BTN, INPUT_PULLUP);
  servoInit();

  wifiBegin();
  routes();
  server.begin();

  logAdd("启动完成, 空闲内存 %u 字节", (unsigned)ESP.getFreeHeap());
  logAdd("抬起=%d° 压下=%d° 按住=%dms", cfg.restAngle, cfg.pressAngle, cfg.holdMs);
  Serial.println(F("提示: USB 串口输入 help 可用命令行配置（不需要 WiFi）"));
}

void loop() {
  server.handleClient();
  servoTick();
  wifiTick();
  buttonTick();
  scheduleTick();
  powerTick();
  cloudTick();
  voiceTick();
  cliTick();

#ifdef SELFTEST
  static bool stDone = false;
  if (!stDone && millis() > 2500) {
    stDone = true;
    selfTest();
  }
#endif

  if (rebootAt && millis() > rebootAt) {
    Serial.println("[reboot]");
    ESP.restart();
  }
  delay(2);
}
