const cloudConfig = require('./cloud-config');

App({
  globalData: {
    baseUrl: '',
    cloudEnabled: false
  },
  onLaunch() {
    const saved = wx.getStorageSync('baseUrl');
    this.globalData.baseUrl = saved || 'http://192.168.4.1';

    // 云开发初始化（没填环境 ID 就跳过，局域网模式照常可用）
    if (wx.cloud && cloudConfig.env) {
      try {
        wx.cloud.init({ env: cloudConfig.env, traceUser: true });
        this.globalData.cloudEnabled = true;
        console.log('[cloud] init ok:', cloudConfig.env);
      } catch (e) {
        console.warn('[cloud] init failed', e);
      }
    } else {
      console.log('[cloud] 未配置环境 ID，云模式不可用');
    }
  }
});
