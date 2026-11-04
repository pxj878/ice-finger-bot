# 云模式：手机不在家也能控制（微信云开发，零域名零服务器）

## 为什么需要它

微信小程序**只能访问 HTTPS 的已备案域名**，`http://192.168.x.x` 在真机上会被拦。
云开发方案用微信自己的域名（已备案），所以**不用买域名、不用买服务器**，还有免费额度。

## 架构

```
[手机小程序] ──wx.cloud.callFunction──► [云函数 icebot] ◄──读写──► [云数据库 icebot_state]
                                              ▲
                                              │ 云开发「HTTP 访问服务」
                                              │ https://<envId>.service.tcloudbase.com/icebot
                                              │
[ESP32] ──每 5 秒 POST 状态 / 取指令──────────┘
```

- 设备**主动轮询**（家宽没有公网 IP，云端推不进去）
- 指令走队列：小程序写 `queue`，设备下次轮询取走并清空
- 延迟 = 轮询间隔（默认 5 秒）

## 一、先本地验证（不用云开发，5 分钟）

固件和协议可以先在一台电脑上跑通：

```powershell
# 1) 电脑上启动 mock 云（就是模拟云函数 + 数据库）
node tools/mock-cloud\server.js 8080

# 2) 看本机 IP，假设是 192.168.1.100
ipconfig

# 3) 串口把设备指过来
cloud http://192.168.1.100:8080/icebot CHANGE_ME_TOKEN
cloud

# 4) 浏览器打开 http://localhost:8080 ，能看到设备状态，点按钮下发指令
```

实测通过的项目：

| 测试 | 结果 |
|---|---|
| 设备轮询上报 | ✅ `HTTP 200`，云端收到 `power/cur/ip/tasks/time` |
| 云端下发 `press` | ✅ 设备执行，状态 `True → False` |
| 云端下发 `angle 70` | ✅ 设备上报 `cur=70` |
| 云端下发定时任务 | ✅ 设备 `tasks=1`，07:30 开机 |

## 二、接入真实微信云开发

### 1. 开通云开发

微信开发者工具 → 顶部「云开发」→ 开通 → 记下 **环境 ID**（形如 `icebot-1g2h3i4j`）。
免费额度对个人够用。

### 2. 填环境 ID

编辑 `miniprogram/cloud-config.js`：

```js
module.exports = {
  env: 'icebot-1g2h3i4j',   // ← 你的环境 ID
  deviceToken: 'CHANGE_ME_TOKEN'     // ← 和设备端一致
};
```

### 3. 建数据库集合

云开发控制台 → 数据库 → 新建集合 **`icebot_state`**，权限选「**仅管理端可读写**」。
（云函数是管理端，小程序通过云函数间接读写，不需要放开客户端权限。）

### 4. 部署云函数

在开发者工具左侧文件树里，右键 `cloudfunctions/icebot` → **「上传并部署：云端安装依赖」**。

可选：云开发控制台 → 云函数 → `icebot` → 配置 → 环境变量，加 `DEVICE_TOKEN=CHANGE_ME_TOKEN`
（不加则默认 `CHANGE_ME_TOKEN`，必须和固件里的一致）。

### 5. 开启 HTTP 访问服务

云开发控制台 → **HTTP 访问服务** → 新建路径 **`/icebot`** → 绑定云函数 `icebot`。

完成后你会得到类似：

```
https://icebot-1g2h3i4j.service.tcloudbase.com/icebot
```

### 6. 配置设备

串口里：

```
cloud https://icebot-1g2h3i4j.service.tcloudbase.com/icebot CHANGE_ME_TOKEN
cloud
```

看到 `成功 N 次 / 失败 0 次, 上次 HTTP=200` 就通了。

### 7. 用小程序

开发者工具里打开「设置 → 远程控制（云模式）」，能看到设备状态、按一下、开机、关机、下发角度。

> 真机上要求：小程序**必须有一个真实 AppID**（测试号不能用云开发），并且已经开通云开发环境。
> 拿到项目后，把 `miniprogram/project.config.json` 里的 `appid` 换成你自己的
> （公开版本里是占位符 `wx0000000000000000`）。

## 三、协议

设备 → 云（POST）：

```json
{
  "token": "CHANGE_ME_TOKEN",
  "action": "sync",
  "dev": "icebot",
  "status": { "power": true, "cur": 21, "rest": 21, "press": 31, "hold": 450,
              "time": 1792332763, "rssi": -61, "ip": "192.168.1.50",
              "ver": "1.0.0", "tasks": 1, "sched": true },
  "tasklist": [ { "enabled": true, "hour": 7, "minute": 30, "days": 127,
                  "action": 0, "once": false, "lastRun": "" } ]
}
```

云 → 设备（同一个响应体返回，返回后队列清空）：

```json
{ "ok": true, "cmd": [ { "a": "press" }, { "a": "angle", "v": 70 },
                       { "a": "tasks", "list": [...] }, { "a": "sched", "v": true } ] }
```

指令类型：`press` / `on` / `off` / `angle`（带 `v`）/ `tasks`（带 `list`）/ `sched`（带 `v`）。

## 四、串口命令

| 命令 | 说明 |
|---|---|
| `cloud` | 查看状态（开关、间隔、URL、成功/失败次数、上次 HTTP 码） |
| `cloud on` / `cloud off` | 开关云轮询 |
| `cloud <url> <token>` | 设置地址与令牌（自动开启） |
| `cloud poll 10` | 改轮询间隔（3~60 秒） |

## 五、限制与注意

1. **延迟 5 秒**：按键后要等设备下次轮询才动。可以 `cloud poll 3` 调快，但更耗电、云端调用次数也更多；
2. **轮询会阻塞**：每次 HTTP 最长 5 秒超时，网络差时舵机动作会稍微延迟；
3. **令牌是弱鉴权**：`CHANGE_ME_TOKEN` 只是防止误触发，别在公开场合透露；
4. **电量**：家里 5V 供电不用管；如果是电池供电，把间隔调到 30~60 秒；
5. 设备离线超过 30 秒，小程序页面会显示「离线」。

## 六、排错

| 现象 | 原因 |
|---|---|
| `cloud` 里失败次数一直涨，HTTP=-1 | URL 不通（设备没联网 / 地址写错 / 电脑防火墙挡了 8080） |
| HTTP=401 | token 不一致（固件 vs 云函数环境变量） |
| HTTP=404 | HTTP 访问服务路径没绑对，注意 `/icebot` |
| 小程序报「未配置云环境」 | `cloud-config.js` 的 env 没填，或没开通云开发 |
| 小程序报「调用云函数失败」 | 云函数没部署 / 名字不是 `icebot` |
| 指令下发后设备不动 | 设备没在轮询（`cloud` 里成功次数不涨），或 token 不同 |
