const api = require('../../utils/api');

const DOW = ['一', '二', '三', '四', '五', '六', '日'];
const ACT = ['开机', '关机', '切换'];

Page({
  data: {
    tasks: [],
    DOW,
    ACT,
    loading: true,
    hint: '',
    sched: true,
    cloudMode: false
  },

  onShow() {
    this.setData({ cloudMode: api.isCloud() });
    this.load();
  },

  load() {
    Promise.all([api.getTasks(), api.getConfig()]).then((res) => {
      const r = res[0];
      const cfg = res[1];
      const tasks = (r.tasks || []).map((t) => this.decorate(t));
      this.setData({
        tasks,
        loading: false,
        hint: '',
        sched: cfg.schedEnabled !== false
      });
    }).catch((e) => {
      this.setData({ loading: false, hint: (e && e.msg) || '读取失败，请检查设备地址' });
    });
  },

  pad(n) {
    return (n < 10 ? '0' : '') + n;
  },

  decorate(t) {
    const hour = typeof t.hour === 'number' ? t.hour : 0;
    const minute = typeof t.minute === 'number' ? t.minute : 0;
    const days = typeof t.days === 'number' ? t.days : 127;
    const dayOn = [0, 1, 2, 3, 4, 5, 6].map((i) => !!(days & (1 << i)));
    return {
      enabled: t.enabled !== false,
      hour,
      minute,
      days,
      action: typeof t.action === 'number' ? t.action : 0,
      once: !!t.once,
      lastRun: t.lastRun || '',
      timeText: this.pad(hour) + ':' + this.pad(minute),
      dayOn
    };
  },

  onEnabled(e) {
    const i = Number(e.currentTarget.dataset.i);
    this.setData({ ['tasks[' + i + '].enabled']: e.detail.value });
  },

  onOnce(e) {
    const i = Number(e.currentTarget.dataset.i);
    this.setData({ ['tasks[' + i + '].once']: e.detail.value });
  },

  onTime(e) {
    const i = Number(e.currentTarget.dataset.i);
    const parts = String(e.detail.value).split(':');
    this.setData({
      ['tasks[' + i + '].hour']: Number(parts[0]),
      ['tasks[' + i + '].minute']: Number(parts[1]),
      ['tasks[' + i + '].timeText']: e.detail.value
    });
  },

  onAction(e) {
    const i = Number(e.currentTarget.dataset.i);
    this.setData({ ['tasks[' + i + '].action']: Number(e.detail.value) });
  },

  onDays(e) {
    const i = Number(e.currentTarget.dataset.i);
    const arr = (e.detail.value || []).map(Number);
    let mask = 0;
    arr.forEach((v) => { mask |= (1 << v); });
    const dayOn = [0, 1, 2, 3, 4, 5, 6].map((k) => !!(mask & (1 << k)));
    this.setData({
      ['tasks[' + i + '].days']: mask,
      ['tasks[' + i + '].dayOn']: dayOn
    });
  },

  onSched(e) {
    if (api.isCloud()) {
      wx.showToast({ title: '云端模式不支持定时总开关', icon: 'none' });
      return;
    }
    const v = e.detail.value;
    this.setData({ sched: v });
    api.setConfig({ schedEnabled: v }).then(() => {
      wx.showToast({ title: v ? '定时已启用' : '定时已停用', icon: 'none' });
    }).catch((err) => {
      wx.showToast({ title: (err && err.msg) || '失败', icon: 'none' });
    });
  },

  add() {
    if (this.data.tasks.length >= 16) {
      wx.showToast({ title: '最多 16 条', icon: 'none' });
      return;
    }
    const t = this.decorate({ enabled: true, hour: 8, minute: 0, days: 127, action: 0, once: false });
    const tasks = this.data.tasks.concat([t]);
    this.setData({ tasks });
  },

  del(e) {
    const i = Number(e.currentTarget.dataset.i);
    const tasks = this.data.tasks.slice();
    tasks.splice(i, 1);
    this.setData({ tasks });
  },

  save() {
    const payload = this.data.tasks.map((t) => ({
      enabled: !!t.enabled,
      hour: t.hour,
      minute: t.minute,
      days: t.days,
      action: t.action,
      once: !!t.once,
      lastRun: t.lastRun || ''
    }));
    wx.showLoading({ title: '保存中', mask: true });
    api.setTasks(payload).then(() => {
      wx.hideLoading();
      wx.showToast({ title: '已保存', icon: 'success' });
      this.load();
    }).catch((err) => {
      wx.hideLoading();
      wx.showToast({ title: (err && err.msg) || '保存失败', icon: 'none' });
    });
  },

  goSettings() {
    wx.switchTab({ url: '/pages/settings/settings' });
  }
});
