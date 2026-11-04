const cloudConfig = require('../../cloud-config');
const app = getApp();

Page({
  data: {
    ready: false,
    loading: true,
    err: '',
    status: {},
    lastSeen: 0,
    offline: true,
    angle: 30,
    busy: false
  },

  onShow() {
    this.poll();
    this.timer = setInterval(() => this.poll(), 3000);
  },

  onHide() {
    clearInterval(this.timer);
  },

  onUnload() {
    clearInterval(this.timer);
  },

  call(action, extra) {
    return new Promise((resolve, reject) => {
      if (!wx.cloud || !cloudConfig.env) {
        reject(new Error('未配置云环境：请先在 cloud-config.js 填 env'));
        return;
      }
      wx.cloud.callFunction({
        name: 'icebot',
        data: Object.assign({ action }, extra || {}),
        success: (res) => {
          const r = res.result || {};
          if (r.ok === false) reject(new Error(r.msg || '云端返回失败'));
          else resolve(r);
        },
        fail: (e) => reject(new Error('调用云函数失败: ' + (e.errMsg || '') + '（云函数是否已部署？）'))
      });
    });
  },

  poll() {
    this.call('get').then((r) => {
      this.setData({
        ready: true,
        loading: false,
        err: '',
        status: r.status || {},
        lastSeen: r.lastSeen || 0,
        offline: !!r.offline
      });
    }).catch((e) => {
      this.setData({ loading: false, ready: false, err: e.message });
    });
  },

  act(e) {
    const a = e.currentTarget.dataset.a;
    this.setData({ busy: true });
    this.call(a).then(() => {
      wx.showToast({ title: '已下发，等设备取指令', icon: 'none' });
      setTimeout(() => this.poll(), 1500);
    }).catch((err) => {
      wx.showToast({ title: err.message, icon: 'none' });
    }).then(() => this.setData({ busy: false }));
  },

  onAngle(e) {
    this.setData({ angle: e.detail.value });
  },

  sendAngle() {
    this.call('angle', { angle: this.data.angle }).then(() => {
      wx.showToast({ title: '已下发 ' + this.data.angle + '°', icon: 'none' });
      setTimeout(() => this.poll(), 1500);
    }).catch((err) => wx.showToast({ title: err.message, icon: 'none' }));
  },

  agoText(ts) {
    if (!ts) return '从未';
    const s = Math.floor((Date.now() - ts) / 1000);
    if (s < 60) return s + ' 秒前';
    if (s < 3600) return Math.floor(s / 60) + ' 分钟前';
    return Math.floor(s / 3600) + ' 小时前';
  }
});
