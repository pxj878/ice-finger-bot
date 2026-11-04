/**
 * 云函数 icebot
 * ------------------------------------------------------------
 * 同时服务两类调用者：
 *   1) 小程序：wx.cloud.callFunction({ name:'icebot', data:{ action:'get' } })
 *   2) 设备：  云开发「HTTP 访问服务」 POST https://<env>.service.tcloudbase.com/icebot
 *
 * 数据库集合：icebot_state（一条文档：_id = 'default'）
 *   { status:{...}, tasklist:[...], queue:[{a,v,list}], lastSeen:number, devid:string }
 *
 * 部署：开发者工具里右键 cloudfunctions/icebot → 「上传并部署：云端安装依赖」
 * 环境变量：DEVICE_TOKEN（可选，默认 CHANGE_ME_TOKEN，必须和固件里 cloud 命令的 token 一致）
 */

const cloud = require('wx-server-sdk');
cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV });

const db = cloud.database();
const _ = db.command;
const COLL = 'icebot_state';
const DOC_ID = 'default';
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'CHANGE_ME_TOKEN';

async function ensureDoc() {
  try {
    return await db.collection(COLL).doc(DOC_ID).get();
  } catch (e) {
    await db.collection(COLL).add({
      data: {
        _id: DOC_ID,
        status: {},
        tasklist: [],
        queue: [],
        lastSeen: 0,
        devid: ''
      }
    });
    return await db.collection(COLL).doc(DOC_ID).get();
  }
}

// ---------- 设备侧（HTTP 访问服务）----------
async function handleDevice(event) {
  let body = {};
  try {
    body = typeof event.body === 'string' ? JSON.parse(event.body) : (event.body || {});
  } catch (e) {
    return { statusCode: 400, body: JSON.stringify({ ok: false, msg: 'bad json' }) };
  }

  if (body.token !== DEVICE_TOKEN) {
    return { statusCode: 401, body: JSON.stringify({ ok: false, msg: 'bad token' }) };
  }

  await ensureDoc();
  const doc = (await db.collection(COLL).doc(DOC_ID).get()).data;
  const cmds = doc.queue || [];

  // 设备上报的状态 + 取出待执行指令，同时清空队列
  await db.collection(COLL).doc(DOC_ID).update({
    data: {
      status: body.status || {},
      tasklist: body.tasklist || [],
      devid: body.dev || '',
      lastSeen: Date.now(),
      queue: []
    }
  });

  return {
    statusCode: 200,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ok: true, cmd: cmds })
  };
}

// ---------- 小程序侧 ----------
async function handleMini(event) {
  const action = event.action || 'get';
  await ensureDoc();
  const doc = (await db.collection(COLL).doc(DOC_ID).get()).data;

  if (action === 'get') {
    return {
      ok: true,
      status: doc.status || {},
      tasklist: doc.tasklist || [],
      lastSeen: doc.lastSeen || 0,
      offline: !doc.lastSeen || Date.now() - doc.lastSeen > 30000
    };
  }

  let item = null;
  if (action === 'press') item = { a: 'press' };
  else if (action === 'on') item = { a: 'on' };
  else if (action === 'off') item = { a: 'off' };
  else if (action === 'angle') item = { a: 'angle', v: Number(event.angle) || 0 };
  else if (action === 'tasks') item = { a: 'tasks', list: event.list || [] };
  else if (action === 'sched') item = { a: 'sched', v: !!event.value };

  if (!item) return { ok: false, msg: '未知 action: ' + action };

  await db.collection(COLL).doc(DOC_ID).update({ data: { queue: _.push([item]) } });
  return { ok: true, queued: item };
}

exports.main = async (event, context) => {
  // HTTP 访问服务会带 httpMethod 字段
  if (event && event.httpMethod) return await handleDevice(event);
  try {
    return await handleMini(event || {});
  } catch (e) {
    return { ok: false, msg: String(e && e.message ? e.message : e) };
  }
};
