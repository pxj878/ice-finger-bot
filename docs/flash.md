# 编译与烧录

## 0. 先确认硬件

| 项目 | 要求 |
|---|---|
| 开发板 | **ESP32-S3**（本项目实测板 4MB Flash；C3 也可用） |
| 数据线 | **能传数据**的 USB-C 线（很多线只能充电，务必换一根试） |
| 舵机 | SG90，5V 独立供电 |
| 电脑 | Windows / macOS / Linux 均可 |

> 确认芯片型号：`python -m esptool --port COM5 chip_id`。
> 本项目**默认环境就是 S3**，`pio run` 即可；C3 加 `-e esp32-c3-devkitm-1`。
>
> ⚠️ **如果烧录成功但板子无限重启**（串口只有 `ESP-ROM` 反复打印、没有任何 app 输出），
> 八成是 **Flash 模式**不对。`chip_id` 里如果看到
> `Embedded Flash 4MB (XMC), Embedded PSRAM 2MB`（Flash+PSRAM 封装在一起的模组），
> 这种模组只认 **DOUT**，用 DIO 会启动失败。`platformio.ini` 里已经配好：
> ```ini
> board_upload.flash_mode = dout   ; 注意是 board_upload，不是 board_build
> board_upload.flash_freq = 40m
> ```
> 换回普通模组如果反而不启动，把这两行注释掉即可。
>
> ⚠️ **Flash 大小一定要对上**：这块 S3 只有 4MB，而 PlatformIO 的 `esp32-s3-devkitc-1`
> 板型默认按 8MB 生成镜像头/分区表，会直接启动循环（串口只有 `ESP-ROM` 反复打印）。
> `platformio.ini` 里已经写了：
> ```ini
> board_upload.flash_size = 4MB
> board_upload.maximum_size = 1310720
> board_build.flash_size = 4MB
> board_build.partitions = default.csv
> ```
> 换板/换型号时这四行要按实际 Flash 大小改。

## 1. 安装工具链（只需一次）

需要 Python 3.9+，然后：

```powershell
pip install -U platformio
```

或者用项目自带脚本（会自动建虚拟环境，不污染系统 Python）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\pio.ps1 -Task build
```

## 2. 找串口

插上开发板，设备管理器里会出现：

- **USB 口（原生 USB，板子标 `USB`）** → 通常显示 `USB 串行设备 (COMx)`，不需要装驱动
- **UART 口（板子标 `UART`）** → CH340/CP210x，Win10+ 一般免驱

记下 `COMx`。也可以用命令：

```powershell
pio device list
```

## 3. 编译

```powershell
cd firmware
pio run
```

首次会下载 ESP32 平台包（几百 MB，需要联网，等几分钟）。

## 4. 烧录

```powershell
cd firmware
pio run -t upload --upload-port COM5
```

如果提示连不上：

1. 按住板子的 **BOOT** 键不放，点一下 **RST**，再松开 BOOT → 进入下载模式，重新执行烧录；
2. 换 USB 线 / 换 USB 口（不要用 USB Hub）；
3. 确认串口没被别的软件（串口助手、Arduino IDE）占用。

## 5. 看日志

```powershell
cd firmware
pio device monitor -p COM5 -b 115200
```

正常启动日志：

```
=== Ice Finger Bot 1.0.0 ===
[   310] 未保存 WiFi, 请连热点后打开 http://192.168.4.1
[   320] 配网热点: IceFingerBot / icebot1234  ->  http://192.168.4.1
[  1200] 启动完成, 空闲内存 214512 字节
[  1201] 抬起=25° 压下=75° 按住=450ms
```

> ⚠️ 默认固件开启了 `ARDUINO_USB_CDC_ON_BOOT`，串口日志走 **USB 口**。
> 如果你用的是 UART 口（CH340）看不到日志，把 `firmware/platformio.ini` 里
> `-DARDUINO_USB_CDC_ON_BOOT=1` 改成 `0` 重新编译烧录即可。

## 6. 验证清单

烧录完成后按顺序验证，全部通过说明硬件没问题：

| # | 操作 | 预期 |
|---|---|---|
| 1 | 上电 | 舵机摆到"抬起角度"（默认 25°） |
| 2 | 串口敲 `status` | 打印固件版本、舵机参数、任务数 |
| 3 | 串口敲 `angle 80` | 舵机转到 80°，说明 PWM 正常 |
| 4 | 手机搜 WiFi | 出现 `IceFingerBot` 热点 |
| 5 | 连热点，浏览器开 `http://192.168.4.1` | 打开配置页，状态显示"待机" |
| 6 | 点「试按一次」 | 舵机压下再抬起，制冰机按钮被按到 |
| 7 | 网页填 WiFi → 保存并重启 | 设备连上路由器，日志出现 IP |
| 8 | 加一条 1 分钟后的定时任务 | 到点自动按下 |
| 9 | 断电再上电 | 角度参数和任务都还在 |

> 第 4、5 步如果手机搜不到热点，先别怀疑固件——用串口命令照样能把设备配好
> （见 [serial-cli.md](serial-cli.md)），热点的问题单独排查（见第 9 节）。

## 7. 常见编译错误

| 报错 | 处理 |
|---|---|
| 报错 `Please specify upload_port` | 加 `--upload-port COMx` |
| `A fatal error occurred: Failed to connect` | 见第 4 节进入下载模式 |
| `A fatal error occurred: This chip is ESP32-S3 not ESP32-C3` | 板子是 S3，加 `-e esp32-s3-devkitc-1` |
| `No such file or directory: 'git'` | 装 Git，或直接在 PlatformIO 里点 Build |
| 编译通过但舵机不转 | 检查信号线是否 GPIO4、舵机是否 5V 供电、是否共地 |
| `ledcSetup was not declared` | 平台版本不对，确认 `platform = espressif32@6.9.0`（本项目已固定） |

---

## 8. 没有路由器也能自检（推荐）

固件内置自检模式：启动后**不开 WiFi**，直接对本机 `192.168.4.1:80` 发 HTTP 请求，
把 Web 服务 / JSON / 定时任务 / 舵机动作整条链路跑一遍，日志直接打在串口上。

```powershell
# 一条命令（自动认芯片、编译、烧录、抓 45 秒日志）
powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -SelfTest

# 或者手动
cd firmware
pio run -c platformio.local.ini -e c3-selftest -t upload --upload-port COM5
pio device monitor -p COM5 -b 115200
```

正常输出见 `docs/verification.md` 第 3 节。

---

## 9. 热点搜不到 / 连不上路由器

先分清两件事：

**A. 手机搜不到 `IceFingerBot` 热点**

> ⚠️ **实测踩过的坑**：如果设备配了 WiFi 但**连不上路由器**，ESP32 底层会不停自动重连，
> 把射频占满，**热点的 beacon 发不出去**，表现就是"手机完全搜不到热点，但串口日志显示热点已开启"。
>
> 固件已修复：关掉自动重连（`WiFi.setAutoReconnect(false)`），改成
> **尝试 15 秒 → 失败就断开静默 → 按 30s/60s/120s… 退避再试**，两次尝试之间热点正常广播。
> 验证方法：清空 WiFi（`reset`）后热点立刻出现；配好 WiFi 且连不上时，热点依然可见。

1. 先用串口确认 AP 起来了：`status` 里看 `AP=192.168.4.1`；
2. 手机离板子近一点（1 米内），关掉手机的「随机 MAC」再试；
3. 如果配了错误的 WiFi：串口 `wifi 错误名 错误密码` 会一直重连，先 `reset` 或 `ap off` 排查；
4. 还搜不到：烧 `firmware/test/ap_probe/` 里的最小 AP 固件（`ZZTEST`），用另一台手机/电脑扫，
   如果别人也扫不到，就是板子的射频/天线问题（部分 C3 小板天线走 U.FL 却没接天线）；
5. **不影响使用**：整个过程都可以用串口命令行完成，见 [serial-cli.md](serial-cli.md)。

**B. 设备连不上家里路由器**

日志里看具体原因码：

| 日志 | 含义 | 处理 |
|---|---|---|
| `Reason: 15 - 4WAY_HANDSHAKE_TIMEOUT` | 关联成功但密码握手超时 | 密码错 / 路由器 WPA3、802.11r 等兼容问题 / 信号太差 |
| `Reason: 2 - AUTH_EXPIRE` | 认证超时，路由器没回应 | 信号太弱、MAC 过滤 |
| `Reason: 202 - AUTH_FAIL` | 路由器拒绝 | MAC 白名单 / 设备接入控制 / 路由器满载 |

排查顺序：

1. 串口 `scan` 看目标 WiFi 的信号强度（低于 −75 dBm 很难稳定）；
2. 把设备挪到离路由器近一点，或换用手机热点验证设备本身；
3. 路由器后台：加密方式改 **纯 WPA2-PSK (AES)**、关掉 **WPA3 / 802.11r 快速漫游 / OFDMA**、
   关掉「无线设备接入控制」，或把设备 MAC 加入白名单；
4. 改完用 `wifi SSID 密码` 重新下发，设备会自动重启重连。

> ⚠️ **同名双 AP 的坑（真实踩过）**：家里如果有两台设备都广播同一个 SSID（比如光猫和 OpenWrt 都叫 `YOUR_WIFI_SSID`），
> 但各自发不同网段的 DHCP（`192.168.1.x` 和 `192.168.2.x`），设备每次重启会**随机连其中一个**：
>
> - 连 A 拿到 `192.168.1.7`，连 B 拿到 `192.168.2.7` —— 表现就是「IP 老变」
> - 如果这时设了静态 IP（例如 `192.168.1.50`），而设备连到了 2.x 那台，就会**完全不通**（网关不在同一网段）
>
> 解决：把两个 AP 的 SSID 分开（或关掉其中一个）。只有一个 `YOUR_WIFI_SSID` 之后，静态 IP 才能稳定工作。

---

## 10. 固定 IP

设备支持两种方式，二选一或都配：

### A. 设备侧静态 IP（不依赖路由器）

串口：

```
ip static 192.168.1.50 255.255.255.0 192.168.1.1 192.168.1.1
```

参数依次是：`IP 掩码 网关 DNS`。重启后生效，`ip` 命令可查看。
电脑/手机上的网页「网络设置 → 使用固定 IP」也能改。

> 前提：这个 IP 必须和**设备实际连的那个 AP** 在同一网段，否则网络直接不通。
> 填错了也别慌：设备热点 `IceFingerBot` 还在，连它打开 `192.168.4.1` 就能改回来（`ip dhcp` 也行）。

### B. 路由器 DHCP 租约（推荐，最稳）

OpenWrt：网络 → DHCP/DNS → 静态地址分配：

| 字段 | 值 |
|---|---|
| 主机名 | icebot |
| MAC | `AA:BB:CC:DD:EE:FF` |
| IPv4 | `192.168.1.50` |

或改 `/etc/config/dhcp`：

```
config host
    option name 'icebot'
    option mac 'AA:BB:CC:DD:EE:FF'
    option ip '192.168.1.50'
```

然后 `/etc/init.d/dnsmasq restart`。设备继续用 DHCP（`ip dhcp`），但永远拿到同一个地址，
不会出现「静态 IP 和网段不匹配」的问题。

### 怎么选

| 场景 | 建议 |
|---|---|
| 只有一个 AP、网段固定 | A 就行，简单 |
| 有多台路由器/AP、或想换设备也自动固定 | **B** 更稳 |


