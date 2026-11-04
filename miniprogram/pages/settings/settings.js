const api = require('../../utils/api');
const bemfa = require('../../utils/bemfa');

Page({
  data: {
    base: '',
    draft: '',
    ssid: '',
    pass: '',
    host: 'icebot',
    tzMin: 480,
    networks: [],
    logs: [],
    fw: '',
    hint: '',
    scanning: false,
    // 连接方式
    mode: 'lan',
    bemfaUid: '',
    bemfaTopic: 'bingji006'
  },

  onShow() {
    const c = bemfa.getCfg();
    this.setData({
      base: api.getBase(),
      draft: api.getBase(),
      appToken: api.getAppToken(),
      mode: api.getMode(),
      bemfaUid: c.uid,
      bemfaTopic: c.topic
    });
    this.loadCfg();
    this.loadLogs();
  },

  // ---- 连接方式 ----
  onMode(e) {
    const m = e.currentTarget.dataset.m;
    api.setMode(m);
    this.setData({ mode: m, hint: '' });
    if (m === 'bemfa') {
      this.loadCfg();
    }
  },

  onBemfaUid(e) {
    this.setData({ bemfaUid: e.detail.value });
  },

  onBemfaTopic(e) {
    this.setData({ bemfaTopic: e.detail.value });
  },

  saveBemfa() {
    if (!this.data.bemfaUid) {
      wx.showToast({ title: '请填写巴法云私钥', icon: 'none' });
      return;
    }
    bemfa.setCfg(this.data.bemfaUid, this.data.bemfaTopic || 'bingji006');
    api.setMode('bemfa');
    this.setData({ mode: 'bemfa', hint: '' });
    wx.showLoading({ title: '测试连接' });
    bemfa.status().then((s) => {
      wx.hideLoading();
      wx.showToast({ title: s.online ? '已连接，设备在线' : '已保存，设备离线', icon: 'none' });
      this.loadCfg();
    }).catch((e) => {
      wx.hideLoading();
      wx.showToast({ title: e.message || '连接失败', icon: 'none' });
    });
  },

  loadCfg() {
    if (api.isCloud()) {
      bemfa.status().then((s) => {
        this.setData({ ssid: '', host: s.name || '', fw: 'bemfa', hint: '' });
      }).catch((e) => {
        this.setData({ hint: (e && e.message) || '云端读取失败' });
      });
      return;
    }
    api.getConfig().then((c) => {
      this.setData({
        ssid: c.ssid || '',
        host: c.host || 'icebot',
        tzMin: typeof c.tzMin === 'number' ? c.tzMin : 480,
        fw: c.fw || '',
        hint: ''
      });
    }).catch((e) => {
      this.setData({ hint: (e && e.msg) || '读取配置失败' });
    });
  },

  onDraft(e) {
    this.setData({ draft: e.detail.value });
  },

  onAppToken(e) {
    this.setData({ appToken: e.detail.value });
    api.setAppToken(e.detail.value);
  },

  saveBase() {
    const u = api.setBase(this.data.draft);
    api.setAppToken(this.data.appToken || '');
    this.setData({ base: u, draft: u });
    wx.showLoading({ title: '测试连接', mask: true });
    api.status().then(() => {
      wx.hideLoading();
      wx.showToast({ title: '连接成功', icon: 'success' });
      this.loadCfg();
    }).catch(() => {
      wx.hideLoading();
      wx.showToast({ title: '连不上，请检查 IP', icon: 'none' });
    });
  },

  scan() {
    this.setData({ scanning: true });
    api.scan().then((r) => {
      this.setData({ networks: r.networks || [], scanning: false });
    }).catch((e) => {
      this.setData({ scanning: false });
      wx.showToast({ title: (e && e.msg) || '扫描失败', icon: 'none' });
    });
  },

  pickNet(e) {
    this.setData({ ssid: e.currentTarget.dataset.ssid });
  },

  onSsid(e) { this.setData({ ssid: e.detail.value }); },
  onPass(e) { this.setData({ pass: e.detail.value }); },
  onHost(e) { this.setData({ host: e.detail.value }); },

  saveWifi() {
    if (!this.data.ssid) {
      wx.showToast({ title: '请填 WiFi 名称', icon: 'none' });
      return;
    }
    wx.showLoading({ title: '保存中', mask: true });
    api.setWifi(this.data.ssid, this.data.pass).then(() => {
      wx.hideLoading();
      wx.showToast({ title: '已保存，设备重启中', icon: 'success' });
    }).catch((e) => {
      wx.hideLoading();
      wx.showToast({ title: (e && e.msg) || '保存失败', icon: 'none' });
    });
  },

  saveTz(e) {
    const tzMin = Number(e.detail.value);
    this.setData({ tzMin });
    api.setConfig({ tzMin }).then(() => {
      wx.showToast({ title: '时区已保存', icon: 'none' });
    }).catch(() => {});
  },

  loadLogs() {
    api.logs().then((r) => {
      this.setData({ logs: r.logs || [] });
    }).catch(() => {});
  },

  reboot() {
    wx.showModal({
      title: '重启设备',
      content: '确定重启 ESP32？',
      success: (res) => {
        if (!res.confirm) return;
        api.reboot().then(() => wx.showToast({ title: '重启中', icon: 'none' })).catch(() => {});
      }
    });
  },

  reset() {
    wx.showModal({
      title: '恢复出厂',
      content: '会清空 WiFi 和所有定时任务，确定？',
      confirmColor: '#f87171',
      success: (res) => {
        if (!res.confirm) return;
        api.reset().then(() => wx.showToast({ title: '已重置', icon: 'none' })).catch(() => {});
      }
    });
  },

  goCloud() {
    wx.navigateTo({ url: '/pages/cloud/cloud' });
  },

  // 预留功能占位
  soon(e) {
    const name = e.currentTarget.dataset.name || '该功能';
    wx.showToast({ title: name + '：预留接口', icon: 'none' });
  }
});
