/**
 * 云开发配置（云模式用）
 * ------------------------------------------------------------
 * 1. 微信开发者工具 → 云开发 → 开通，记下「环境 ID」（形如 icebot-1g2h3i4j）
 * 2. 把环境 ID 填到下面的 env
 * 3. 设备令牌 token 要和固件里 `cloud <url> <token>` 的 token 一致
 *    （云函数环境变量 DEVICE_TOKEN 也要同名同值）
 */
module.exports = {
  env: '',            // 例如 'icebot-1g2h3i4j'（微信云开发，二选一）
  deviceToken: 'CHANGE_ME_TOKEN',
  // 自建服务器方案（VPS + 域名）：
  // 把下面的 appToken 填成服务器上 APP_TOKEN 的值，并在「设置」页把设备地址改成 https://你的域名
  appToken: ''
};
