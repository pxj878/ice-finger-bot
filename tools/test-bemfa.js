/**
 * 用 Node 模拟小程序的 wx 环境，真实调用巴法云 API，
 * 验证 miniprogram/utils/bemfa.js 的逻辑是否正确。
 *
 * 用法：node tools/test-bemfa.js <uid> [topic]
 */

const https = require('https');
const path = require('path');

const UID = process.argv[2];
const TOPIC = process.argv[3] || 'bingji006';
if (!UID) {
  console.error('用法: node tools/test-bemfa.js <uid> [topic]');
  process.exit(1);
}

// ---------- 模拟 wx ----------
const store = { bemfaUid: UID, bemfaTopic: TOPIC, bemfaType: 3 };

global.wx = {
  getStorageSync: (k) => store[k],
  setStorageSync: (k, v) => { store[k] = v; },
  request(opts) {
    const u = new URL(opts.url);
    if (opts.method === 'GET' && opts.data) {
      Object.keys(opts.data).forEach((k) => u.searchParams.set(k, opts.data[k]));
    }
    const body = opts.method === 'POST' ? JSON.stringify(opts.data || {}) : null;
    const req = https.request(
      {
        hostname: u.hostname,
        path: u.pathname + u.search,
        method: opts.method || 'GET',
        headers: Object.assign(
          { 'Content-Type': 'application/json' },
          body ? { 'Content-Length': Buffer.byteLength(body) } : {}
        ),
        timeout: 8000
      },
      (res) => {
        let buf = '';
        res.setEncoding('utf8');
        res.on('data', (c) => (buf += c));
        res.on('end', () => {
          let data;
          try { data = JSON.parse(buf); } catch (e) { data = buf; }
          opts.success({ statusCode: res.statusCode, data });
        });
      }
    );
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', (e) => opts.fail({ errMsg: e.message }));
    if (body) req.write(body);
    req.end();
  }
};

const bemfa = require(path.resolve(__dirname, '../miniprogram/utils/bemfa.js'));

let pass = 0, fail = 0;
const ok = (name, cond, detail = '') => {
  if (cond) { pass++; console.log('  ✓ ' + name + (detail ? '  ' + detail : '')); }
  else { fail++; console.log('  ✗ ' + name + (detail ? '  ' + detail : '')); }
};

(async () => {
  console.log('== 巴法云 API 联调（小程序逻辑层）==');
  console.log('   UID:', UID.slice(0, 8) + '...', ' 主题:', TOPIC);

  // 1. 状态
  const s = await bemfa.status();
  ok('读取状态', typeof s.power === 'boolean', `power=${s.power} online=${s.online} name=${s.name} type=${s.deviceType}`);

  // 2. 定时列表
  const before = await bemfa.getTasks();
  ok('读取定时列表', Array.isArray(before.tasks), `现有 ${before.tasks.length} 条`);

  // 3. 写入一条测试任务（07:30 开，周一到周五）
  const r = await bemfa.setTasks([{ enabled: true, hour: 7, minute: 30, days: 31, action: 0 }]);
  ok('写入定时任务', r.ok && r.count === 1, `count=${r.count}`);

  // 4. 读回校验
  const after = await bemfa.getTasks();
  const t = after.tasks[0];
  ok('读回任务内容', after.tasks.length === 1 && t.hour === 7 && t.minute === 30 && t.action === 0,
    `time=${t && t.timeText} days=${t && t.days} action=${t && t.action}`);

  // 5. 星期掩码转换（周一到周五 = bit0..4 = 31 -> 巴法 [1,2,3,4,5] -> 回来还是 31）
  ok('星期转换往返一致', t && t.days === 31, `days=${t && t.days}（期望 31）`);

  // 6. 清空
  const c = await bemfa.setTasks([]);
  ok('清空定时任务', c.ok, `count=${c.count}`);
  const empty = await bemfa.getTasks();
  ok('确认已清空', empty.tasks.length === 0, `剩余 ${empty.tasks.length} 条`);

  // 7. 开关（会真的按一下按钮）
  if (process.argv.includes('--press')) {
    const a = await bemfa.action('on');
    ok('下发开机', a.ok && a.power === true, JSON.stringify(a));
    await new Promise((r2) => setTimeout(r2, 3000));
    const off = await bemfa.action('off');
    ok('下发关机', off.ok && off.power === false, JSON.stringify(off));
  } else {
    console.log('  - 跳过开关测试（加 --press 参数可测，会真的按按钮）');
  }

  console.log(`\n结果: ${pass} 通过 / ${fail} 失败`);
  process.exit(fail ? 1 : 0);
})().catch((e) => {
  console.error('致命错误:', e.message || e);
  process.exit(1);
});
