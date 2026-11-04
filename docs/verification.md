# 验证记录

> 环境：Windows 10 / PlatformIO Core 6.1.19 / espressif32 6.9.0 (Arduino core 2.0.17)
> 主力板卡：**ESP32-S3**（Adafruit ESP32-S3，**4MB Flash**），USB-Serial-JTAG `303A:1001`，串口 COM6
> 参考板卡：ESP32-C3（**射频发射异常，已弃用**，详见第 6 节）

## 1. 编译

| 目标 | 结果 |
|---|---|
| `esp32-s3-devkitc-1`（默认，4MB 覆盖） | ✅ SUCCESS，RAM 14.7% |
| `esp32-c3-devkitm-1`（备选） | ✅ SUCCESS，RAM 13.0%，Flash 67.7% |

## 2. 烧录 + 启动

```
=== Ice Finger Bot 1.0.0 ===
[    1202] 配网热点 IceFingerBot 已开启 -> http://192.168.4.1
[    1202] 未保存 WiFi, 请连热点后打开 http://192.168.4.1
[    1205] mDNS 已启动: http://icebot.local
[    1207] 启动完成, 空闲内存 268608 字节
[    1319] 抬起=25° 压下=75° 按住=450ms
提示: USB 串口输入 help 可用命令行配置（不需要 WiFi）
```

> 第一版按 8MB 板型烧录，启动时直接循环重启（`Detected size(4096k) smaller than the size in the
> binary image header(8192k)`）。把 flash_size / 分区表改成 4MB 后正常，见第 5 节问题 #6。

## 3. 真实空口验证（笔记本连设备热点）

```
netsh wlan show interfaces
    State   : connected
    SSID    : IceFingerBot
    BSSID   : a2:f2:62:f5:5b:54
    Signal  : 75%
```

HTTP 全部在**真实 WiFi** 上跑（不是本机 loopback）：

| 请求 | 结果 |
|---|---|
| `GET /api/config` | 200, 176 B |
| `POST /api/config`（改角度） | 200 `{"ok":true,"msg":"已保存"}` |
| `POST /api/tasks`（2 条任务） | 200 `{"ok":true,"count":2}` |
| `GET /api/tasks` | 200, 194 B |
| `POST /api/action {"action":"angle","angle":60}` | 200 `{"ok":true,"angle":60}` |
| `POST /api/action {"action":"press"}` | 200 `{"ok":true,"power":true}` |
| `GET /api/logs` | 200, 354 B |
| `GET /` | 200, **15205 B**（完整网页） |
| `GET /api/status` | 200, 272 B |

## 4. 免 WiFi 自检（SELFTEST）

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -SelfTest
```

```
[    2501] ========== 自检开始 ==========
[    2535] GET  /api/status   -> HTTP/1.1 200 OK | 545 bytes
[    2909] POST /api/config   -> HTTP/1.1 200 OK | 283 bytes
[    3088] POST /api/tasks    -> HTTP/1.1 200 OK | 293 bytes
[    3265] GET  /api/tasks    -> HTTP/1.1 200 OK | 362 bytes
[    3446] POST /api/action   -> HTTP/1.1 200 OK | 278 bytes
[    3623] POST /api/action   -> HTTP/1.1 200 OK | 277 bytes
[    3812] GET  /api/logs     -> HTTP/1.1 200 OK | 1011 bytes
[   14155] GET  /             -> HTTP/1.1 200 OK | 10318 bytes
[   14336] 已建定时任务 18:21, 等待自动触发...
[   24423] 执行任务 #1 (18:21)
[   24423] 按键触发(定时任务), 待执行 1
[   30341] 等待结束: 电源 1 -> 0 (toggle 应该翻转)
[   30341] ========== 自检结束 ==========
```

## 5. 测试中发现并修复的问题

| # | 问题 | 现象 | 修复 |
|---|---|---|---|
| 1 | LEDC 分辨率 16bit 在 C3 上不合法 | `ledcSetup(): ... maximum 14` | 改 14 bit（S3 也兼容） |
| 2 | 两个 4KB `StaticJsonDocument` 叠加爆栈 | POST `/api/tasks` → `Guru Meditation`，重启循环 | 全改 `DynamicJsonDocument`（堆分配） |
| 3 | 首次启动 NVS 无 key 打 `[E] nvs_get_str` | 日志噪音 | `prefs.isKey()` 先判断 |
| 4 | 动作状态机写死起止角度 | 手动摆过角度后再按，从错误起点插值 | 通用 `servoMoveTo(from→to)` + `PH_MANUAL` |
| 5 | 串口 `task list` 栈溢出 | 7 个中文星期名（3 字节/个）塞进 16 字节缓冲 → `Stack smashing` 重启 | 缓冲区改 32 字节 |
| 6 | 4MB 板子用了 8MB 镜像头/分区表 | 只有 `ESP-ROM` 反复打印，启动循环 | `board_upload.flash_size/board_build.partitions` 按 4MB 设置 |
| 7 | **WiFi 密码没有持久化** | 网页里填完密码能连上，**一重启就掉**，之后一直 `4WAY_HANDSHAKE_TIMEOUT`（拿空密码握手） | `cfgToJson()` 补 `pass` 字段（只写 NVS，HTTP 响应里不下发） |
| 8 | STA 反复自动重连占满射频 | 配了连不上的 WiFi 后，手机**完全搜不到热点**，但串口日志显示热点已开启 | 关自动重连，改「尝试 15s → 失败静默 → 30/60/120s 退避」 |

> 第 2、5、6、7、8 条都会导致「设备不可用」级别的现象，全部已修复并复测。

## 6. 关于那块 ESP32-C3（已弃用）

同一份代码在 C3 上的表现：

| 测试 | 结果 |
|---|---|
| 开热点 | `softAP ok=1`，`AP=192.168.4.1`，**但笔记本和手机都扫不到**（最小固件 `ZZTEST` + wlanapi 强制扫描同样扫不到） |
| 连家里路由器 | `AUTH_EXPIRE` 为主，偶发 `AUTH_FAIL(202)` |
| 接收 | 正常，能扫到 7 个网络（YOUR_WIFI_SSID −50 dBm） |

换成 S3 后热点立刻可见、网页立刻可用 → **确认是那块 C3 板子的射频发射问题，不是固件问题**。

## 7. 连家里 WiFi 的现状

S3 连 YOUR_WIFI_SSID 一直 `4WAY_HANDSHAKE_TIMEOUT`（密码握手超时），而笔记本用同一密码能连上。

笔记本在同一位置测到 YOUR_WIFI_SSID 只有 **−81 dBm**，属于边缘信号。建议按顺序试：

1. 把设备挪到离路由器近一点；
2. 用手机开热点给设备连（更快验证）；
3. 路由器后台检查「无线设备接入控制 / MAC 过滤」，把 `AA:BB:CC:DD:EE:FF` 加入白名单。

连上后会自动 NTP 对时，网页/小程序即可远程使用。

## 8. 你自己复测的最短路径

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -SelfTest   # 编译+烧录+自检+存日志
pio device monitor -p COM6 -b 115200                                  # 串口，输入 help
```

> 自检会改写配置和任务（角度、一条 18:21 的任务），正式使用前重新校准一次即可。
> `firmware/platformio.local.ini` 里有明文 WiFi 密码，已加入 `.gitignore`。
