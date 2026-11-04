/**
 * 巴法云 API 封装（小程序直连，不需要自建服务器/域名）
 * ------------------------------------------------------------
 * 接口（全部 HTTPS，需在微信后台把 https://apis.bemfa.com 加入 request 合法域名）：
 *   状态   GET  /vb/api/v2/topicInfo?openID=&type=&topic=
 *   在线   GET  /va/online?uid=&topic=&type=
 *   开关   POST /va/postJsonMsg      { uid, topic, type, msg }
 *   定时   GET  /vb/delay/v1/timeList?openID=&topicID=&type=
 *          POST /vb/delay/v1/addTime { openID, topicID, type, time, msg, week }
 *          POST /vb/delay/v1/deleteTime { openID, topicID, type, id }
 *
 * 约定：uid / topic 存在本地 Storage，不写死在代码里。
 */

const BASE = 'https://apis.bemfa.com';
const TYPE = 3; // 1=MQTT设备  3=TCP设备

function getCfg() {
  return {
    uid: wx.getStorageSync('bemfaUid') || '',
    topic: wx.getStorageSync('bemfaTopic') || 'bingji006',
    type: Number(wx.getStorageSync('bemfaType') || TYPE)
  };
}

function setCfg(uid, topic) {
  wx.setStorageSync('bemfaUid', String(uid || '').trim());
  if (topic) wx.setStorageSync('bemfaTopic', String(topic).trim());
  wx.setStorageSync('bemfaType', TYPE);
}

function hasCfg() {
  const c = getCfg();
  return !!(c.uid && c.topic);
}

function req(path, method, data) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: BASE + path,
      method: method || 'GET',
      data: data || {},
      timeout: 8000,
      header: { 'Content-Type': 'application/json' },
      success(res) {
        const b = res.data || {};
        if (res.statusCode >= 200 && res.statusCode < 300 && (b.code === 0 || b.code === undefined)) {
          resolve(b);
        } else {
          reject(new Error(b.msg || b.message || ('HTTP ' + res.statusCode)));
        }
      },
      fail(err) {
        reject(new Error('网络错误: ' + ((err && err.errMsg) || '') + '（域名是否已加入白名单？）'));
      }
    });
  });
}

// ---------- 日期转换：我们 bit0=周一..bit6=周日；巴法 0=周日,1=周一..6=周六 ----------
const WEEK_MAP = [1, 2, 3, 4, 5, 6, 0];

function maskToWeek(mask) {
  const out = [];
  for (let i = 0; i < 7; i++) if (mask & (1 << i)) out.push(WEEK_MAP[i]);
  return out.length ? out : [0, 1, 2, 3, 4, 5, 6];
}

function weekToMask(week) {
  let mask = 0;
  (week || []).forEach((w) => {
    const i = WEEK_MAP.indexOf(Number(w));
    if (i >= 0) mask |= (1 << i);
  });
  return mask || 0x7f;
}

const pad = (n) => (n < 10 ? '0' + n : '' + n);

// ---------- 对外接口 ----------

// 状态：返回和局域网模式兼容的字段
async function status() {
  const c = getCfg();
  if (!c.uid) throw new Error('未配置巴法云私钥');
  const info = await req(`/vb/api/v2/topicInfo?openID=${encodeURIComponent(c.uid)}&type=${c.type}&topic=${encodeURIComponent(c.topic)}`);
  const d = info.data || {};
  return {
    ok: true,
    mode: '云端',
    fw: 'bemfa',
    ip: '-',
    ssid: '',
    rssi: 0,
    power: d.msg === 'on',
    online: !!d.online,
    tasks: 0,
    cur: -1,
    rest: 0,
    press: 0,
    holdMs: 0,
    timeOk: false,
    time: d.time || '-',
    name: d.name || c.topic,
    deviceType: d.deviceType || ''
  };
}

// 开关：on / off / press（press = 读当前状态后取反）
async function action(a) {
  if (a === 'on' || a === 'off') {
    await push(a);
    return { ok: true, power: a === 'on' };
  }
  if (a === 'press') {
    const s = await status();
    const next = s.power ? 'off' : 'on';
    await push(next);
    return { ok: true, power: next === 'on' };
  }
  throw new Error('云端模式只能开/关');
}

function push(msg) {
  const c = getCfg();
  return req('/va/postJsonMsg', 'POST', { uid: c.uid, topic: c.topic, type: c.type, msg });
}

async function listTimers() {
  const c = getCfg();
  const r = await req(`/vb/delay/v1/timeList?openID=${encodeURIComponent(c.uid)}&topicID=${encodeURIComponent(c.topic)}&type=${c.type}`);
  const arr = (r.data && r.data.data) || [];
  return arr.map((t) => {
    const [h, m] = String(t.time || '00:00:00').split(':');
    return {
      id: t.id,
      enabled: Number(t.status) === 1,
      hour: Number(h),
      minute: Number(m),
      days: weekToMask(t.week),
      action: t.msg === 'off' ? 1 : 0,
      once: false,
      timeText: pad(Number(h)) + ':' + pad(Number(m))
    };
  });
}

async function addTimer(task) {
  const c = getCfg();
  const time = pad(task.hour) + ':' + pad(task.minute) + ':00';
  const msg = task.action === 1 ? 'off' : 'on';
  return req('/vb/delay/v1/addTime', 'POST', {
    openID: c.uid,
    topicID: c.topic,
    type: c.type,
    time,
    msg,
    week: maskToWeek(task.days)
  });
}

async function delTimer(id) {
  const c = getCfg();
  return req('/vb/delay/v1/deleteTime', 'POST', {
    openID: c.uid,
    topicID: c.topic,
    type: c.type,
    id
  });
}

// 整体覆盖（先删后加，任务数很少，够用）
async function setTasks(list) {
  const old = await listTimers();
  for (const t of old) {
    try { await delTimer(t.id); } catch (e) {}
  }
  let n = 0;
  for (const t of list) {
    if (!t.enabled) continue;
    if (t.action === 2) continue; // 云端定时不支持"切换"，跳过
    await addTimer(t);
    n++;
  }
  return { ok: true, count: n };
}

async function getTasks() {
  const timers = await listTimers();
  return { ok: true, tasks: timers };
}

module.exports = {
  getCfg,
  setCfg,
  hasCfg,
  status,
  action,
  getTasks,
  setTasks,
  listTimers,
  addTimer,
  delTimer
};
