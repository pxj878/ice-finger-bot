/**
 * 制冰机手指机器人 - 云服务器中转
 * ------------------------------------------------------------
 * 零依赖，只用 Node 内置模块。作用：
 *
 *   设备 --POST /device/sync（每 N 秒）--> 本服务 <--GET/POST /api/*-- 小程序 / 网页
 *
 * 设备侧协议与固件 cloud 轮询完全一致（见 docs/cloud.md）；
 * 服务端把设备上报的状态存下来，对小程序暴露和固件一样的 /api/* 接口，
 * 所以网页和小程序只要把地址改成 https://你的域名 即可，代码不用动。
 *
 * 环境变量：
 *   PORT          监听端口，默认 8080
 *   DEVICE_TOKEN  设备令牌（固件里 cloud <url> <token> 的 token）
 *   APP_TOKEN     小程序/网页令牌（请求头 X-App-Token）
 *   DATA_FILE     状态文件，默认 ./data/state.json
 *   ALLOW_ORIGIN  CORS 允许来源，默认 *
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const PORT = Number(process.env.PORT || 8080);
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'dev-token-change-me';
const APP_TOKEN = process.env.APP_TOKEN || 'app-token-change-me';
const DATA_FILE = process.env.DATA_FILE || path.join(__dirname, 'data', 'state.json');
const ALLOW_ORIGIN = process.env.ALLOW_ORIGIN || '*';
const OFFLINE_MS = 45 * 1000;

// ---------------- 状态存储 ----------------
let state = {
  device: {
    id: '',
    status: {},
    tasklist: [],
    lastSeen: 0,
    polls: 0
  },
  queue: []
};

function loadState() {
  try {
    const raw = fs.readFileSync(DATA_FILE, 'utf8');
    const s = JSON.parse(raw);
    if (s && s.device) state = s;
  } catch (e) {
    // 首次运行没有文件，忽略
  }
}

let saveTimer = null;
function saveState() {
  if (saveTimer) return;
  saveTimer = setTimeout(() => {
    saveTimer = null;
    try {
      fs.mkdirSync(path.dirname(DATA_FILE), { recursive: true });
      const tmp = DATA_FILE + '.tmp';
      fs.writeFileSync(tmp, JSON.stringify(state, null, 2));
      fs.renameSync(tmp, DATA_FILE);  // 原子替换，避免写一半断电
    } catch (e) {
      console.error('[store] 保存失败', e.message);
    }
  }, 200);
}

// ---------------- 小工具 ----------------
function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Access-Control-Allow-Origin': ALLOW_ORIGIN,
    'Access-Control-Allow-Headers': 'Content-Type, X-App-Token',
    'Access-Control-Allow-Methods': 'GET,POST,OPTIONS',
    'Cache-Control': 'no-store'
  });
  res.end(body);
}

function readBody(req, limit = 64 * 1024) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => {
      const raw = Buffer.concat(chunks).toString('utf8');
      if (!raw) return resolve({});
      try {
        resolve(JSON.parse(raw));
      } catch (e) {
        reject(new Error('bad json'));
      }
    });
    req.on('error', reject);
  });
}

function safeEqual(a, b) {
  const ba = Buffer.from(String(a || ''));
  const bb = Buffer.from(String(b || ''));
  if (ba.length !== bb.length) return false;
  return crypto.timingSafeEqual(ba, bb);
}

function appOk(req) {
  const t = req.headers['x-app-token'] || new URL(req.url, 'http://x').searchParams.get('token');
  return safeEqual(t, APP_TOKEN);
}

function deviceOnline() {
  return state.device.lastSeen && Date.now() - state.device.lastSeen < OFFLINE_MS;
}

function pushCmd(cmd) {
  state.queue.push(cmd);
  if (state.queue.length > 50) state.queue = state.queue.slice(-50);
  saveState();
}

// ---------------- 路由 ----------------
async function handleDevice(req, res) {
  let body;
  try {
    body = await readBody(req);
  } catch (e) {
    return json(res, 400, { ok: false, msg: e.message });
  }
  if (!safeEqual(body.token, DEVICE_TOKEN)) {
    console.warn('[device] token 不匹配');
    return json(res, 401, { ok: false, msg: 'bad token' });
  }

  state.device.id = body.dev || state.device.id;
  state.device.status = body.status || {};
  state.device.tasklist = body.tasklist || [];
  state.device.lastSeen = Date.now();
  state.device.polls++;

  const cmds = state.queue;
  state.queue = [];
  saveState();

  if (cmds.length) console.log('[device] 下发指令', JSON.stringify(cmds));
  return json(res, 200, { ok: true, cmd: cmds });
}

function mirrorStatus() {
  const st = state.device.status || {};
  return Object.assign({}, st, {
    ok: true,
    online: deviceOnline(),
    lastSeen: state.device.lastSeen,
    polls: state.device.polls,
    pending: state.queue.length,
    mode: deviceOnline() ? 'SERVER(online)' : 'SERVER(offline)',
    fw: (st.ver || '') + '-srv'
  });
}

async function handleApi(req, res, pathname, query) {
  if (!appOk(req)) return json(res, 401, { ok: false, msg: '缺少或错误的 X-App-Token' });

  // GET /api/status
  if (pathname === '/api/status' && req.method === 'GET') return json(res, 200, mirrorStatus());

  // GET /api/tasks
  if (pathname === '/api/tasks' && req.method === 'GET') {
    return json(res, 200, { ok: true, tasks: state.device.tasklist || [] });
  }

  // GET /api/config  （只镜像设备上报的字段）
  if (pathname === '/api/config' && req.method === 'GET') {
    const st = state.device.status || {};
    return json(res, 200, {
      ok: true,
      ssid: '',
      hasPass: false,
      host: state.device.id || 'icebot',
      restAngle: st.rest || 0,
      pressAngle: st.press || 0,
      holdMs: st.hold || 0,
      schedEnabled: st.sched !== false,
      powerSave: st.powerSave,
      online: deviceOnline()
    });
  }

  if (req.method === 'POST') {
    let body;
    try {
      body = await readBody(req);
    } catch (e) {
      return json(res, 400, { ok: false, msg: e.message });
    }

    // POST /api/action  {action: press|on|off|angle, angle?}
    if (pathname === '/api/action') {
      const a = body.action;
      if (a === 'press' || a === 'on' || a === 'off') {
        pushCmd({ a });
        return json(res, 200, { ok: true, queued: a, pending: state.queue.length });
      }
      if (a === 'angle') {
        const v = Math.max(0, Math.min(180, Number(body.angle) || 0));
        pushCmd({ a: 'angle', v });
        return json(res, 200, { ok: true, queued: 'angle', angle: v });
      }
      return json(res, 400, { ok: false, msg: 'action 只能是 press/on/off/angle' });
    }

    // POST /api/tasks  {tasks:[...]}
    if (pathname === '/api/tasks') {
      const list = Array.isArray(body.tasks) ? body.tasks.slice(0, 16) : null;
      if (!list) return json(res, 400, { ok: false, msg: '缺少 tasks 数组' });
      pushCmd({ a: 'tasks', list });
      return json(res, 200, { ok: true, queued: 'tasks', count: list.length });
    }

    // POST /api/config  {schedEnabled?}
    if (pathname === '/api/config') {
      if (typeof body.schedEnabled === 'boolean') {
        pushCmd({ a: 'sched', v: body.schedEnabled });
        return json(res, 200, { ok: true, queued: 'sched' });
      }
      return json(res, 200, { ok: true, msg: '云模式下只支持 schedEnabled' });
    }

    // 兼容：/api/time 直接忽略
    if (pathname === '/api/time') return json(res, 200, { ok: true, msg: '设备自行 NTP 对时' });

    return json(res, 404, { ok: false, msg: 'not found' });
  }

  return json(res, 404, { ok: false, msg: 'not found' });
}

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://localhost');
  const p = u.pathname;

  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': ALLOW_ORIGIN,
      'Access-Control-Allow-Headers': 'Content-Type, X-App-Token',
      'Access-Control-Allow-Methods': 'GET,POST,OPTIONS'
    });
    return res.end();
  }

  if (p === '/healthz') return json(res, 200, { ok: true, up: process.uptime() });

  // 设备轮询
  if (req.method === 'POST' && (p === '/device/sync' || p === '/icebot')) {
    return handleDevice(req, res);
  }

  if (p.startsWith('/api/')) {
    try {
      return await handleApi(req, res, p, u.searchParams);
    } catch (e) {
      console.error('[api]', e);
      return json(res, 500, { ok: false, msg: 'server error' });
    }
  }

  // 根路径给个简单状态页（方便浏览器直接看）
  if (p === '/' && req.method === 'GET') {
    const st = mirrorStatus();
    const rows = Object.entries(st)
      .filter(([k]) => ['online', 'power', 'cur', 'rest', 'press', 'tasks', 'lastSeen', 'polls', 'pending'].includes(k))
      .map(([k, v]) => `<tr><td>${k}</td><td>${typeof v === 'object' ? JSON.stringify(v) : v}</td></tr>`)
      .join('');
    const body = `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>IceBot 服务器</title>
<style>body{background:#0e1116;color:#e6ebf2;font:15px/1.6 -apple-system,"PingFang SC",sans-serif;padding:20px;max-width:720px;margin:0 auto}
table{width:100%;border-collapse:collapse}td{padding:6px;border-bottom:1px solid #252c38}td:first-child{color:#8b97a8;width:40%}
.card{background:#171c24;border:1px solid #252c38;border-radius:14px;padding:16px;margin:14px 0}
.sub{color:#8b97a8;font-size:13px}</style></head><body>
<h1>IceBot 云中转</h1>
<div class="sub">设备${st.online ? '在线' : '离线'} · 最后上报 ${st.lastSeen ? new Date(st.lastSeen).toLocaleString('zh-CN') : '从未'}</div>
<div class="card"><table>${rows}</table></div>
<div class="sub">设备令牌与小程序令牌请勿泄露；接口需要 X-App-Token。</div>
</body></html>`;
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Access-Control-Allow-Origin': ALLOW_ORIGIN });
    return res.end(body);
  }

  return json(res, 404, { ok: false, msg: 'not found' });
});

loadState();
server.listen(PORT, '0.0.0.0', () => {
  console.log('=== IceBot 云中转已启动 ===');
  console.log('  监听端口   :', PORT);
  console.log('  设备轮询   : POST http://<域名>/device/sync');
  console.log('  小程序接口 : GET  http://<域名>/api/status   (需 X-App-Token)');
  console.log('  状态文件   :', DATA_FILE);
  if (DEVICE_TOKEN === 'dev-token-change-me' || APP_TOKEN === 'app-token-change-me') {
    console.warn('  ⚠️ 正在使用默认令牌，正式部署前务必改成随机串！');
  }
});
