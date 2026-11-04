/**
 * 小程序端自动化回归测试（mini-e2e）
 * ------------------------------------------------------------
 * 前提：
 *   1. 微信开发者工具已打开本项目，并开启了自动化端口
 *      （tools\mini-e2e\README.md 有一步步说明）
 *   2. 电脑连着设备热点 IceFingerBot（或改 DEV 指向设备 IP）
 *
 * 用法：
 *   node tools/mini-e2e/mini-e2e.js
 *   node tools/mini-e2e/mini-e2e.js --no-press     # 不真的按舵机
 *   node tools/mini-e2e/mini-e2e.js --keep-tasks   # 不改动现有任务
 *
 * 环境变量：
 *   MINI_WS       自动化 ws 地址，默认 ws://127.0.0.1:9421
 *   MINI_CLI      微信开发者工具 cli.bat 路径（自动拉起 automation 时用）
 *   MINI_PROJECT  小程序项目目录，默认 ../../miniprogram
 *   DEVICE_URL    设备地址，默认 http://192.168.4.1
 */

const automator = require('miniprogram-automator');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const WS = process.env.MINI_WS || 'ws://127.0.0.1:9421';
const CLI = process.env.MINI_CLI || 'C:\\Program Files (x86)\\Tencent\\微信web开发者工具\\cli.bat';
const PROJ = process.env.MINI_PROJECT || path.resolve(__dirname, '../../miniprogram');
const DEVICE = process.env.DEVICE_URL || 'http://192.168.4.1';

const ARGS = process.argv.slice(2);
const NO_PRESS = ARGS.includes('--no-press');
const KEEP_TASKS = ARGS.includes('--keep-tasks');

const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
const OUT_DIR = path.resolve(__dirname, '../../logs/mini-e2e-' + stamp);

let pass = 0;
let fail = 0;
const failures = [];

function ok(name, cond, detail = '') {
  if (cond) {
    pass++;
    console.log('  \u2713 ' + name + (detail ? '  ' + detail : ''));
  } else {
    fail++;
    failures.push(name + (detail ? '  ' + detail : ''));
    console.log('  \u2717 ' + name + (detail ? '  ' + detail : ''));
  }
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

function httpJson(p, method = 'GET', body) {
  return new Promise((resolve, reject) => {
    const data = body ? Buffer.from(JSON.stringify(body)) : null;
    const req = http.request(DEVICE + p, {
      method,
      timeout: 8000,
      headers: data
        ? { 'Content-Type': 'application/json', 'Content-Length': data.length }
        : {}
    }, (res) => {
      let buf = '';
      res.setEncoding('utf8');
      res.on('data', (c) => (buf += c));
      res.on('end', () => {
        try { resolve(JSON.parse(buf)); } catch (e) { reject(new Error('非 JSON: ' + buf.slice(0, 80))); }
      });
    });
    req.on('timeout', () => req.destroy(new Error('超时')));
    req.on('error', reject);
    if (data) req.write(data);
    req.end();
  });
}

async function shot(mp, name) {
  try {
    const out = path.join(OUT_DIR, name);
    const r = await mp.screenshot({ path: out });
    if (typeof r === 'string' && r.length > 1000) fs.writeFileSync(out, Buffer.from(r, 'base64'));
    return fs.existsSync(out) ? out : null;
  } catch (e) {
    return null;
  }
}

async function tapByText(page, keyword) {
  const buttons = await page.$$('button');
  for (const b of buttons) {
    const t = (await b.text()).trim();
    if (t.includes(keyword)) { await b.tap(); return t; }
  }
  return null;
}

async function connect() {
  try {
    return await automator.connect({ wsEndpoint: WS });
  } catch (e) {
    console.log('  ! 连不上自动化端口，尝试用 cli 拉起 automation ...');
    try {
      execFileSync('cmd', ['/c', '"' + CLI + '" auto --project "' + PROJ + '" --auto-port 9421'],
        { stdio: 'ignore', timeout: 120000 });
      await sleep(6000);
      return await automator.connect({ wsEndpoint: WS });
    } catch (e2) {
      throw new Error('无法连接自动化端口，请先在开发者工具里运行 cli auto --auto-port 9421');
    }
  }
}

(async () => {
  fs.mkdirSync(OUT_DIR, { recursive: true });
  console.log('== 制冰机手指机器人 · 小程序回归测试 ==');
  console.log('   设备:', DEVICE, ' 自动化:', WS, ' 截图目录:', OUT_DIR);

  // ---------- 1. 设备在线 ----------
  let dev0;
  try {
    dev0 = await httpJson('/api/status');
    ok('设备在线', !!dev0.ok, 'fw=' + dev0.fw + ' power=' + dev0.power + ' cur=' + dev0.cur);
  } catch (e) {
    ok('设备在线', false, e.message);
    console.log('\n设备不可达，先检查电脑是否连着 IceFingerBot 热点。');
    process.exit(1);
  }

  const taskCount0 = dev0.tasks;
  const cur0 = dev0.cur;

  // ---------- 2. 连接模拟器 ----------
  const mp = await connect();
  ok('连接开发者工具自动化端口', true, WS);

  // ---------- 3. 控制页 ----------
  let page = await mp.currentPage();
  if (!/pages\/index/.test(page.path)) {
    page = await mp.reLaunch('/pages/index/index');
    await sleep(1500);
  }
  await sleep(1200);
  let d = await page.data();
  ok('控制页读到设备数据', !!(d.status && d.status.fw), 'base=' + d.base + ' fw=' + (d.status || {}).fw);
  ok('控制页角度与设备一致', d.rest === dev0.rest && d.press === dev0.press,
    'rest=' + d.rest + '/' + dev0.rest + ' press=' + d.press + '/' + dev0.press);
  ok('控制页无错误提示', !d.hint, d.hint || '');
  await shot(mp, '01-index.png');

  // ---------- 4. 点击「按一下」 ----------
  if (!NO_PRESS) {
    const before = (await page.data()).power;
    const t = await tapByText(page, '按一下');
    ok('找到并点击「按一下」', !!t, t || '');
    await sleep(4200);
    const after = (await page.data()).power;
    ok('点击后电源状态翻转', before !== after, before + ' -> ' + after);
    const dev1 = await httpJson('/api/status');
    ok('设备侧状态同步', dev1.power === after, 'device=' + dev1.power + ' ui=' + after);
    await shot(mp, '02-after-press.png');
  } else {
    console.log('  - 跳过「按一下」（--no-press）');
  }

  // ---------- 5. 舵机预览 ----------
  try {
    await tapByText(page, '预览抬起');
    await sleep(1500);
    const dev2 = await httpJson('/api/status');
    ok('预览抬起角生效', dev2.cur === dev2.rest, 'cur=' + dev2.cur + ' rest=' + dev2.rest);
  } catch (e) {
    ok('预览抬起角生效', false, e.message);
  }

  // ---------- 6. 定时任务增删 ----------
  if (!KEEP_TASKS) {
    await mp.switchTab('/pages/schedule/schedule');
    await sleep(1600);
    page = await mp.currentPage();
    const sd0 = await page.data();
    ok('定时页加载', Array.isArray(sd0.tasks), '现有 ' + (sd0.tasks || []).length + ' 条');

    await tapByText(page, '添加任务');
    await sleep(500);
    const sd1 = await page.data();
    ok('新增任务到列表', (sd1.tasks || []).length === (sd0.tasks || []).length + 1,
      (sd0.tasks || []).length + ' -> ' + (sd1.tasks || []).length);

    await tapByText(page, '保存');
    await sleep(2200);
    const dev3 = await httpJson('/api/tasks');
    ok('保存后设备收到任务', dev3.tasks.length === taskCount0 + 1,
      'device=' + dev3.tasks.length + ' 期望=' + (taskCount0 + 1));
    await shot(mp, '03-schedule.png');

    await tapByText(page, '删除');
    await sleep(500);
    await tapByText(page, '保存');
    await sleep(2200);
    const dev4 = await httpJson('/api/tasks');
    ok('删除后设备任务恢复', dev4.tasks.length === taskCount0,
      'device=' + dev4.tasks.length + ' 期望=' + taskCount0);
  } else {
    console.log('  - 跳过任务增删（--keep-tasks）');
  }

  // ---------- 7. 设置页 ----------
  await mp.switchTab('/pages/settings/settings');
  await sleep(1600);
  page = await mp.currentPage();
  const st = await page.data();
  ok('设置页加载', st.base === DEVICE, 'base=' + st.base);
  await shot(mp, '04-settings.png');

  // ---------- 8. 收尾：把舵机放回原角度 ----------
  try {
    if (!NO_PRESS) {
      await httpJson('/api/action', 'POST', { action: 'angle', angle: cur0 });
    }
  } catch (e) { /* 忽略 */ }

  await mp.disconnect();

  console.log('\n=================================');
  console.log(' 通过: ' + pass + '   失败: ' + fail);
  if (fail) {
    console.log(' 失败项:');
    failures.forEach((f) => console.log('   - ' + f));
  }
  console.log(' 截图: ' + OUT_DIR);
  console.log('=================================');
  process.exit(fail ? 1 : 0);
})().catch((e) => {
  console.error('\n[致命错误] ' + (e && e.message ? e.message : e));
  process.exit(1);
});
