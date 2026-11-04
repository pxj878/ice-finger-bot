/**
 * 设备接口统一封装
 * ------------------------------------------------------------
 * 现在直连局域网 http://<设备IP>。
 * 将来接云端（MQTT / 中转服务器）时，只需要改 request() 的实现，
 * 各页面的调用代码不用动。
 */

const DEFAULT_BASE = 'http://192.168.4.1';
const cloudConfig = require('../cloud-config');
const bemfa = require('./bemfa');

// ---------------- 连接方式：lan(局域网直连) | bemfa(巴法云直连) ----------------
function getMode() {
  return wx.getStorageSync('connMode') || 'lan';
}

function setMode(m) {
  wx.setStorageSync('connMode', m === 'bemfa' ? 'bemfa' : 'lan');
}

const isCloud = () => getMode() === 'bemfa';

function unsupportedInCloud(what) {
  return Promise.reject({ ok: false, msg: what + ' 仅支持局域网模式' });
}

function getBase() {
  return wx.getStorageSync('baseUrl') || DEFAULT_BASE;
}

function getAppToken() {
  return wx.getStorageSync('appToken') || cloudConfig.appToken || '';
}

function setAppToken(t) {
  wx.setStorageSync('appToken', String(t || '').trim());
}

function setBase(url) {
  let u = String(url || '').trim().replace(/\/+$/, '');
  if (u && !/^https?:\/\//i.test(u)) u = 'http://' + u;
  wx.setStorageSync('baseUrl', u || DEFAULT_BASE);
  return u || DEFAULT_BASE;
}

function request(path, method, data) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: getBase() + path,
      method: method || 'GET',
      data: data || {},
      header: {
        'Content-Type': 'application/json',
        'X-App-Token': getAppToken()
      },
      timeout: 6000,
      success(res) {
        const body = res.data || {};
        if (res.statusCode >= 200 && res.statusCode < 300 && body.ok !== false) {
          resolve(body);
        } else {
          reject({ ok: false, msg: body.msg || ('HTTP ' + res.statusCode) });
        }
      },
      fail(err) {
        reject({ ok: false, msg: '网络错误: ' + ((err && err.errMsg) || '') });
      }
    });
  });
}

module.exports = {
  // ---- 基础 ----
  getBase,
  setBase,
  getAppToken,
  setAppToken,
  request,
  getMode,
  setMode,
  isCloud,

  // ---- 状态 ----
  status: () => (isCloud() ? bemfa.status() : request('/api/status')),

  // ---- 配置（云端模式不支持）----
  getConfig: () => (isCloud() ? unsupportedInCloud('读取配置') : request('/api/config')),
  setConfig: (cfg) => (isCloud() ? unsupportedInCloud('修改配置') : request('/api/config', 'POST', cfg)),

  // ---- 定时任务（云端模式走巴法定时接口）----
  getTasks: () => (isCloud() ? bemfa.getTasks() : request('/api/tasks')),
  setTasks: (tasks) => (isCloud() ? bemfa.setTasks(tasks) : request('/api/tasks', 'POST', { tasks })),

  // ---- 动作 ----
  action: (a) => (isCloud() ? bemfa.action(a) : request('/api/action', 'POST', { action: a })),
  // 舵机直接转到某个角度（校准用）
  setAngle: (angle) =>
    isCloud() ? unsupportedInCloud('舵机调角') : request('/api/action', 'POST', { action: 'angle', angle }),

  // ---- 时间 ----
  setTime: (epoch, tzMin) => request('/api/time', 'POST', { epoch, tzMin }),

  // ---- 网络 ----
  scan: () => request('/api/scan'),
  setWifi: (ssid, pass) => request('/api/wifi', 'POST', { ssid, pass }),

  // ---- 运维 ----
  logs: () => request('/api/logs'),
  reboot: () => request('/api/reboot', 'POST', {}),
  reset: () => request('/api/reset', 'POST', {}),

  /* ============================================================
   * 预留接口（对应固件 /api/v1/*，当前固件返回 501，仅占位）
   * 后续扩展直接在这里补实现即可
   * ============================================================ */

  // OTA：上传固件
  ota: (filePath) => new Promise((resolve, reject) => {
    wx.uploadFile({
      url: getBase() + '/api/v1/ota',
      filePath,
      name: 'firmware',
      success: (res) => resolve(res.data),
      fail: reject
    });
  }),

  // MQTT：配置云端中转
  setMqtt: (cfg) => request('/api/v1/mqtt', 'POST', cfg),

  // 传感器：门磁 / 温度 / 电流
  sensor: () => request('/api/v1/sensor'),

  // 设备信息：型号、能力集
  deviceInfo: () => request('/api/v1/device'),

  // 红外遥控（预留）
  ir: (code) => request('/api/v1/ir', 'POST', { code })
};
