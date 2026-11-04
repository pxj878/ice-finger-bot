# 小程序自动化回归测试（mini-e2e）

一条命令跑完控制页 → 校准 → 定时任务 → 设置页，并校验**设备侧真实状态**，最后截图存档。

```
✓ 设备在线  fw=1.0.0 power=false cur=21
✓ 控制页读到设备数据
✓ 控制页角度与设备一致  rest=21/21 press=31/31
✓ 点击后电源状态翻转  false -> true
✓ 设备侧状态同步  device=true ui=true
✓ 预览抬起角生效  cur=21 rest=21
✓ 新增任务到列表  0 -> 1
✓ 保存后设备收到任务  device=1
✓ 删除后设备任务恢复  device=0
通过: 14   失败: 0
```

## 1. 一次性准备

### 1.1 打开开发者工具的服务端口

微信开发者工具 → 右上角 **设置 ⚙ → 安全设置 → 服务端口 → 开启**。

> 如果找不到这个开关（2.x 改版后入口有变化），可以直接改配置：
> `%LOCALAPPDATA%\微信开发者工具\User Data\<hash>\WeappLocalData\localstorage_*.json`
> 里把 `"enableServicePort":false` 改成 `true`，并在同目录 `Default\` 下放一个内容为 `On` 的 `.ide-status`。

### 1.2 用 CLI 打开项目并开启自动化

```powershell
$cli = 'C:\Program Files (x86)\Tencent\微信web开发者工具\cli.bat'
$proj = 'C:\path\to\ice-finger-bot\miniprogram'

& cmd /c "`"$cli`" open --project `"$proj`" --port 9420"
& cmd /c "`"$cli`" auto --project `"$proj`" --auto-port 9421"
```

成功会看到 `IDE server has started, listening on http://127.0.0.1:9420` 和 `{"autoPort":9421}`。

### 1.3 安装依赖

```powershell
cd tools\mini-e2e
npm install
```

### 1.4 设备就位

电脑连上设备热点 **`IceFingerBot`**（密码 `icebot1234`），确认能打开 `http://192.168.4.1`。

## 2. 跑测试

```powershell
cd ice-finger-bot
node tools\mini-e2e\mini-e2e.js
```

| 参数 | 说明 |
|---|---|
| `--no-press` | 不真的驱动舵机（只读页面数据） |
| `--keep-tasks` | 不改动现有定时任务 |

环境变量（都在脚本顶部有默认值）：`MINI_WS`、`MINI_CLI`、`MINI_PROJECT`、`DEVICE_URL`。

退出码 0 = 全通过，1 = 有失败项（可直接接 CI/批处理）。

## 3. 它会检查什么

| 分组 | 检查点 |
|---|---|
| 设备 | `/api/status` 可达、固件号 |
| 控制页 | 页面数据来自真实设备、角度与设备一致、无错误提示 |
| 动作 | 点「按一下」→ 页面状态翻转，且**设备侧状态同步** |
| 校准 | 点「预览抬起」→ 设备 `cur` 等于 `rest` |
| 定时 | 新增 → 保存 → 设备任务 +1 → 删除 → 保存 → 设备任务复原 |
| 设置 | 设备地址正确 |
| 截图 | `logs/mini-e2e-<时间>/01-index.png` 等 4 张 |

## 4. 注意

- 测试会**真的按一次舵机**（除非 `--no-press`），制冰机会被开/关一次；
- 跑完会把舵机角度放回原值、任务数恢复原状；
- 改完 wxss/wxml 后，开发者工具**不会热更新**，要 `cli close` + `cli open` 重开项目；
- 端口 9420/9421 被占用时，先关掉旧的 IDE 进程。
