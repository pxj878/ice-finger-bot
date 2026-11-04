const api = require('../../utils/api');

Page({
  data: {
    status: {},
    power: false,
    busy: false,
    hint: '',
    base: '',
    // 舵机校准
    rest: 25,
    press: 75,
    holdMs: 450,
    cur: 0
  },

  onLoad() {
    this.setData({ base: api.getBase() });
    this.loadConfig();
  },

  onShow() {
    this.refresh();
    this.timer = setInterval(() => this.refresh(), 3000);
  },

  onHide() {
    clearInterval(this.timer);
  },

  onUnload() {
    clearInterval(this.timer);
  },

  onPullDownRefresh() {
    this.refresh().then(() => wx.stopPullDownRefresh());
  },

  refresh() {
    return api.status().then((s) => {
      this.setData({
        status: s,
        power: !!s.power,
        busy: !!s.busy,
        cur: typeof s.cur === 'number' ? s.cur : 0,
        hint: '',
        base: api.getBase()
      });
    }).catch((e) => {
      this.setData({ hint: (e && e.msg) || '设备无响应，请检查 WiFi 和 IP 设置' });
    });
  },

  // 首次进页面时把设备里的角度读出来
  loadConfig() {
    if (api.isCloud()) return;  // 云端模式没有角度配置
    api.getConfig().then((c) => {
      this.setData({
        rest: typeof c.restAngle === 'number' ? c.restAngle : 25,
        press: typeof c.pressAngle === 'number' ? c.pressAngle : 75,
        holdMs: typeof c.holdMs === 'number' ? c.holdMs : 450
      });
    }).catch(() => {});
  },

  act(e) {
    const a = e.currentTarget.dataset.a;
    wx.showLoading({ title: '执行中', mask: true });
    api.action(a).then(() => {
      wx.hideLoading();
      wx.showToast({ title: '已执行', icon: 'success' });
      setTimeout(() => this.refresh(), 700);
    }).catch((err) => {
      wx.hideLoading();
      wx.showToast({ title: (err && err.msg) || '失败', icon: 'none' });
    });
  },

  // ---- 舵机校准 ----
  onRestDrag(e) {
    // 拖动过程中只更新显示，不发请求
    this.setData({ rest: e.detail.value });
  },

  onRest(e) {
    this.setData({ rest: e.detail.value });
    this.previewAngle(e.detail.value);
  },

  onPress(e) {
    this.setData({ press: e.detail.value });
    this.previewAngle(e.detail.value);
  },

  onRestStep(e) {
    const d = Number(e.currentTarget.dataset.d);
    const v = Math.max(0, Math.min(120, this.data.rest + d));
    this.setData({ rest: v });
    this.previewAngle(v);
  },

  onPressStep(e) {
    const d = Number(e.currentTarget.dataset.d);
    const v = Math.max(10, Math.min(180, this.data.press + d));
    this.setData({ press: v });
    this.previewAngle(v);
  },

  onHold(e) {
    this.setData({ holdMs: e.detail.value });
  },

  previewAngle(angle) {
    api.setAngle(angle).then((r) => {
      this.setData({ cur: r.angle });
    }).catch((err) => {
      wx.showToast({ title: (err && err.msg) || '舵机无响应', icon: 'none' });
    });
  },

  previewRest() {
    this.previewAngle(this.data.rest);
  },

  previewPress() {
    this.previewAngle(this.data.press);
  },

  saveServo() {
    wx.showLoading({ title: '保存中', mask: true });
    api.setConfig({
      restAngle: this.data.rest,
      pressAngle: this.data.press,
      holdMs: this.data.holdMs
    }).then(() => {
      wx.hideLoading();
      wx.showToast({ title: '已保存', icon: 'success' });
    }).catch((err) => {
      wx.hideLoading();
      wx.showToast({ title: (err && err.msg) || '保存失败', icon: 'none' });
    });
  },

  testPress() {
    this.saveServo();
    setTimeout(() => this.act({ currentTarget: { dataset: { a: 'press' } } }), 400);
  },

  syncTime() {
    const epoch = Math.floor(Date.now() / 1000);
    const tzMin = -new Date().getTimezoneOffset();
    api.setTime(epoch, tzMin).then(() => {
      wx.showToast({ title: '已对时', icon: 'success' });
      this.refresh();
    }).catch((e) => {
      wx.showToast({ title: (e && e.msg) || '对时失败', icon: 'none' });
    });
  },

  goSettings() {
    wx.switchTab({ url: '/pages/settings/settings' });
  },

  goSchedule() {
    wx.switchTab({ url: '/pages/schedule/schedule' });
  }
});
