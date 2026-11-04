/**
 * 本地 mock 云服务（不依赖任何 npm 包）
 * ------------------------------------------------------------
 * 作用：在没有微信云开发环境时，模拟"云函数 + 数据库"，用来验证固件的云轮询链路。
 *
 * 启动：
 *   node tools/mock-cloud/server.js 8080
 *
 * 设备端配置（串口）：
 *   cloud http://192.168.1.100:8080/icebot CHANGE_ME_TOKEN
 *   cloud on
 *
 * 浏览器打开 http://localhost:8080 可以看到设备上报的状态，并点按钮下发指令。
 */

const http = require('http');
const url = require('url');

const PORT = Number(process.argv[2] || 8080);
const TOKEN = process.env.TOKEN || 'CHANGE_ME_TOKEN';

// 单设备状态（多设备可按 dev 分开存，这里简化）
let state = {
  dev: null,
  status: {},
  tasklist: [],
  lastSeen: null,
  polls: 0
};
let queue = [];   // 待下发的指令
let history = []; // 最近指令记录

function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Access-Control-Allow-Origin': '*'
  });
  res.end(body);
}

function html(res) {
  const s = state.status || {};
  const rows = Object.entries(s).map(([k, v]) =>
    `<tr><td>${k}</td><td>${typeof v === 'object' ? JSON.stringify(v) : v}</td></tr>`).join('');
  const page = `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mock 云 · 制冰机手指机器人</title>
<style>
body{background:#0e1116;color:#e6ebf2;font:15px/1.6 -apple-system,"PingFang SC",sans-serif;padding:16px;max-width:720px;margin:0 auto}
h1{font-size:19px}.card{background:#171c24;border:1px solid #252c38;border-radius:14px;padding:14px;margin:12px 0}
button{font:inherit;background:#202836;color:#e6ebf2;border:1px solid #252c38;border-radius:10px;padding:9px 14px;margin:4px;cursor:pointer}
button.p{background:#38bdf8;color:#04121c;border-color:#38bdf8;font-weight:700}
table{width:100%;border-collapse:collapse}td{padding:4px 6px;border-bottom:1px solid #1d232d;font-size:14px}
td:first-child{color:#8b97a8;width:38%}
.sub{color:#8b97a8;font-size:12.5px}
</style></head><body>
<h1>Mock 云服务</h1>
<div class="sub">设备最近上报：${state.lastSeen || '从未'} · 轮询次数 ${state.polls} · 待执行指令 ${queue.length}</div>
<div class="card"><b>设备状态</b><table>${rows || '<tr><td colspan=2>还没有数据</td></tr>'}</table></div>
<div class="card"><b>下发指令</b><div>
<button class="p" onclick="cmd('press')">按一下</button>
<button onclick="cmd('on')">开机</button>
<button onclick="cmd('off')">关机</button>
<button onclick="cmd('angle',60)">舵机 60°</button>
</div><div class="sub">指令会在设备下次轮询（约 5 秒）时被取走</div></div>
<div class="card"><b>最近指令</b><div class="sub">${history.slice(-8).reverse().join('<br>') || '无'}</div></div>
<script>
async function cmd(a, v) {
  const q = '/cmd?a=' + a + (v !== undefined ? '&v=' + v : '');
  await fetch(q);
  setTimeout(() => location.reload(), 300);
}
setInterval(() => location.reload(), 6000);
</script></body></html>`;
  res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
  res.end(page);
}

const server = http.createServer((req, res) => {
  const u = url.parse(req.url, true);

  // 设备轮询入口
  if (req.method === 'POST' && (u.pathname === '/icebot' || u.pathname === '/')) {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => {
      let data;
      try { data = JSON.parse(body || '{}'); } catch (e) { return json(res, 400, { ok: false, msg: 'bad json' }); }
      if (data.token !== TOKEN) {
        console.log('[auth] token 不匹配:', data.token);
        return json(res, 401, { ok: false, msg: 'bad token' });
      }
      state.dev = data.dev || state.dev;
      state.status = data.status || {};
      state.tasklist = data.tasklist || [];
      state.lastSeen = new Date().toLocaleTimeString('zh-CN');
      state.polls++;

      const cmds = queue.slice();
      queue = [];
      if (cmds.length) console.log('[poll] 下发', JSON.stringify(cmds));
      json(res, 200, { ok: true, cmd: cmds });
    });
    return;
  }

  // 网页/测试接口：加指令
  if (u.pathname === '/cmd') {
    const a = u.query.a;
    const v = u.query.v;
    if (!a) return json(res, 400, { ok: false });
    const item = { a };
    if (v !== undefined) item.v = Number(v);

    // 演示下发一组定时任务：/cmd?a=tasks&h=7&m=30
    if (a === 'tasks') {
      item.list = [{
        enabled: true,
        hour: Number(u.query.h || 7),
        minute: Number(u.query.m || 30),
        days: 127,
        action: Number(u.query.act || 0),
        once: false
      }];
    }

    queue.push(item);
    history.push(`${new Date().toLocaleTimeString('zh-CN')}  ${JSON.stringify(item)}`);
    return json(res, 200, { ok: true, queued: queue.length });
  }

  // 状态 JSON（给脚本用）
  if (u.pathname === '/state') return json(res, 200, { ...state, queue });

  if (u.pathname === '/favicon.ico') { res.writeHead(204); return res.end(); }

  html(res);
});

server.listen(PORT, '0.0.0.0', () => {
  console.log('mock 云服务已启动');
  console.log('  设备轮询地址: http://<本机IP>:' + PORT + '/icebot');
  console.log('  token:        ' + TOKEN);
  console.log('  浏览器控制台: http://localhost:' + PORT + '/');
  console.log('  串口配置:     cloud http://<本机IP>:' + PORT + '/icebot ' + TOKEN);
});
