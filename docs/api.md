# HTTP API 接口文档

- 基址：`http://<设备IP>/api`（如 `http://192.168.4.1/api`）
- 数据格式：请求与响应均为 `application/json; charset=utf-8`
- 已开启 CORS（`Access-Control-Allow-Origin: *`），本地 HTML 文件也能直接调用
- 所有响应都带 `"ok": true|false`；失败时 `msg` 为中文原因

---

## 1. 状态

### `GET /api/status`

```json
{
  "ok": true,
  "fw": "1.0.0",
  "name": "icebot",
  "mode": "STA+AP",
  "ip": "192.168.1.23",
  "apIp": "192.168.4.1",
  "ssid": "MyHome",
  "rssi": -57,
  "uptime": 1284,
  "heap": 214512,
  "power": false,
  "busy": false,
  "timeOk": true,
  "time": "2026-10-18 18:20:32",
  "epoch": 1760782832,
  "tzMin": 480,
  "tasks": 2,
  "sched": true,
  "rest": 25,
  "press": 75,
  "holdMs": 450
}
```

| 字段 | 说明 |
|---|---|
| `power` | 逻辑电源状态（固件记录，非真实反馈） |
| `busy` | 舵机是否正在动作 |
| `timeOk` | 是否已获取到有效时间 |
| `mode` | `STA+AP` / `AP` / `STA` |

---

## 2. 配置

### `GET /api/config`

```json
{
  "ssid": "MyHome", "hasPass": true, "host": "icebot",
  "schedEnabled": true,
  "restAngle": 25, "pressAngle": 75, "holdMs": 450,
  "tzMin": 480, "powerOn": false, "apAlways": true,
  "fw": "1.0.0", "mode": "STA+AP"
}
```

### `POST /api/config`

请求体只需带要修改的字段（其余保持不变）：

```json
{ "restAngle": 30, "pressAngle": 80, "holdMs": 500 }
```

| 字段 | 范围 | 说明 |
|---|---|---|
| `ssid` | ≤32 字节 | 修改后设备会自动重启 |
| `pass` | ≤64 字节 | **留空表示不修改** |
| `host` | ≤31 字节 | mDNS 名称 |
| `schedEnabled` | bool | 定时总开关 |
| `restAngle` | 0–180 | 抬起角度 |
| `pressAngle` | 0–180 | 压下角度 |
| `holdMs` | 50–5000 | 按住时长(ms) |
| `tzMin` | -720–840 | 时区分钟，东八区 480 |
| `apAlways` | bool | 是否常开配网热点 |

响应：`{"ok":true,"msg":"已保存"}`

---

## 3. 定时任务

### `GET /api/tasks`

```json
{
  "ok": true,
  "tasks": [
    { "enabled": true, "hour": 8,  "minute": 0,  "days": 127, "action": 0, "once": false, "lastRun": "2026-10-18 08:00" },
    { "enabled": true, "hour": 22, "minute": 30, "days": 127, "action": 1, "once": false, "lastRun": "" }
  ]
}
```

| 字段 | 说明 |
|---|---|
| `hour` / `minute` | 24 小时制，本地时间（受 `tzMin` 影响） |
| `days` | 位图：bit0=周一 … bit6=周日，`127`(0x7F)=每天 |
| `action` | `0`=开机 `1`=关机 `2`=切换(按一下) |
| `once` | 单次任务，执行后 `enabled` 自动置 false |
| `lastRun` | 上次执行时间（防重复执行） |

### `POST /api/tasks`

**整体替换**（最多 16 条）：

```json
{
  "tasks": [
    { "enabled": true, "hour": 8, "minute": 0, "days": 127, "action": 0, "once": false }
  ]
}
```

响应：`{"ok":true,"msg":"已保存","count":1}`

> 新增/删除/修改都是"读出数组 → 改 → 整体写回"，小程序端实现最简单。

---

## 4. 动作

### `POST /api/action`

```json
{ "action": "press" }
```

| action | 行为 |
|---|---|
| `press` | 无条件按一下（切换） |
| `on` | 若当前逻辑状态是"关"→ 按一下，并记为开机 |
| `off` | 若当前逻辑状态是"开"→ 按一下，并记为关机 |
| `angle` | 校准用：`{"action":"angle","angle":45}` 把舵机直接摆到指定角度并**保持**，不自动回位 |

响应：`{"ok":true,"power":true}`
（舵机是异步动作，请求立即返回，`/api/status` 的 `busy` 反映是否在动）

`angle` 的响应：`{"ok":true,"angle":45}`

> 网页端「舵机校准」里的「预览抬起角 / 预览压下角 / 回 0° / 到 180°」就是调这个接口，
> 可以边接线边用手机微调，确认手指正好压到按钮。

---

## 5. 时间

### `POST /api/time`

```json
{ "epoch": 1760782832, "tzMin": 480 }
```

手动对时（设备没联网时用浏览器时间灌进去）。`epoch` 为 Unix 秒。

---

## 6. WiFi 配网

### `GET /api/scan`

```json
{ "ok": true, "networks": [ { "ssid": "MyHome", "rssi": -45, "enc": true } ] }
```

### `POST /api/wifi`

```json
{ "ssid": "MyHome", "pass": "12345678" }
```

保存后 1 秒自动重启。

---

## 7. 运维

| 接口 | 说明 |
|---|---|
| `GET /api/logs` | 最近 24 条运行日志 `{"ok":true,"logs":["..."]}` |
| `POST /api/reboot` | 重启设备 |
| `POST /api/reset` | 清空配置与任务（恢复出厂）后重启 |

---

## 8. 预留扩展接口（后续版本实现）

这些路径已注册，目前统一返回：

```json
{ "ok": false, "msg": "预留接口, 待后续版本实现" }
```

| 路径 | 规划用途 |
|---|---|
| `POST /api/v1/ota` | 固件远程升级（分片上传） |
| `POST /api/v1/mqtt` | 配置 MQTT 服务器，接入云平台/小程序远程控制 |
| `GET  /api/v1/sensor` | 预留传感器（门磁、温度、电流检测） |
| `POST /api/v1/ir` | 预留红外发射（直接遥控制冰机） |
| `POST /api/v1/voice` | 预留语音播报/提示音 |
| `GET  /api/v1/device` | 设备信息扩展（型号、序列号、能力集） |

### 设计约定（给小程序/服务端）

1. 新接口一律放在 `/api/v1/` 下，保持老接口不变；
2. 响应结构保持 `{"ok":bool,"msg":string}` + 业务字段；
3. 需要鉴权时通过请求头 `X-Device-Token`，固件预留了字段但当前不校验；
4. 小程序真机要求 HTTPS + 备案域名，方案见 `README.md`「远程控制路线」。
