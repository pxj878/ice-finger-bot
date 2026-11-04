# 微信小程序

制冰机手指机器人的手机端控制台。支持**局域网直连**和**巴法云远程**两种模式。

## 1. 打开方式

1. 微信开发者工具 → 导入项目 → 选择 `miniprogram/` 目录
2. **AppID**：`project.config.json` 里是占位符 `wx0000000000000000`，换成你自己的
   （没有的话可以点「使用测试号」；但云开发/正式发布必须用真实 AppID）
3. 详情 → 本地设置 → 勾选 **「不校验合法域名、web-view（业务域名）、TLS 版本以及 HTTPS 证书」**
   （只在局域网调试时需要；用巴法云模式不需要关）
4. 手机和电脑连同一个 WiFi（或手机连 `IceFingerBot` 热点）
5. 「设置」页选连接方式：
   - **局域网直连**：填设备 IP（配网时是 `192.168.4.1`）
   - **巴法云**：填巴法云私钥 + 主题（出门也能用）

> 真机预览时微信会强制校验 HTTPS 域名，所以局域网 HTTP 只能"开发者工具 + 真机调试"；
> 要在手机上正常用，走巴法云模式（域名 `https://apis.bemfa.com` 需加入 request 合法域名）。

## 2. 页面结构

| 页面 | 功能 |
|---|---|
| `pages/index` | 设备状态、按一下 / 开机 / 关机、对时 |
| `pages/schedule` | 定时任务（局域网用设备本地定时；云端用巴法定时） |
| `pages/settings` | 连接方式、设备地址、巴法云私钥、WiFi 配网、时区、日志、恢复出厂 |
| `pages/cloud` | 云开发（微信云开发方案）调试页 |

## 3. 目录

```
miniprogram/
├── app.js / app.json / app.wxss     全局配置与样式
├── utils/api.js                     接口封装（局域网/巴法云双模式）
├── utils/bemfa.js                   巴法云 API 封装
├── pages/index/                     控制页
├── pages/schedule/                  定时任务页
└── pages/settings/                  设置页
```

## 4. 接口约定

全部走 `utils/api.js`，页面不直接写 `wx.request`。这样以后从"局域网直连"切到"云端中转"，只改 `request()` 一处。

```js
api.getBase()                 // 读设备地址
api.setBase('192.168.1.23')   // 存设备地址
api.status()                  // 状态
api.action('press'|'on'|'off')// 动作
api.getTasks() / api.setTasks(tasks)
api.getConfig() / api.setConfig({...})
api.setTime(epoch, tzMin)
api.scan() / api.setWifi(ssid, pass)
api.logs() / api.reboot() / api.reset()
```

### 预留接口（当前固件返回 501）

```js
api.ota(filePath)      // POST /api/v1/ota     固件升级
api.setMqtt(cfg)       // POST /api/v1/mqtt    云端中转配置
api.sensor()           // GET  /api/v1/sensor  门磁/温度/电流
api.deviceInfo()       // GET  /api/v1/device  型号与能力集
api.ir(code)           // POST /api/v1/ir      红外遥控
```

## 5. 远程控制的三种路线

小程序**真机**要求请求域名必须是已备案的 HTTPS 域名，所以在家里局域网直接用 `http://192.168.x.x` 只在开发者工具里可行。要真正远程用，选一条：

| 路线 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. 内网穿透** | frp / ngrok / 花生壳 把设备 80 端口映射到公网域名 + HTTPS | 不用改固件 | 家里要常开一台中转设备 |
| **B. 云平台 MQTT** | 设备连 EMQX / 阿里云 IoT，小程序调云 API | 稳定、可多设备、可做用户体系 | 需要写一点云端服务 |
| **C. 自建服务器** | 一台 VPS 跑 HTTP + WebSocket，设备长连接 | 完全可控 | 要维护服务器 |

固件已经把 `/api/v1/mqtt` 的位置留好了；路线 B 不需要改动现有 `/api/*` 接口，小程序端只要把 `utils/api.js` 的 `request()` 换成云端地址即可。

## 6. 待办

- [ ] 接入云端（MQTT）实现真机远程
- [ ] 舵机角度校准页（现在在网页端）
- [ ] 操作历史 / 按键次数统计
- [ ] 多设备管理
- [ ] 用户登录与权限
