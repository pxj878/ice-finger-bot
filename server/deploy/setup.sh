#!/usr/bin/env bash
# ============================================================
#  IceBot 云中转 - Ubuntu 22.04 一键部署脚本
#  用法（在服务器上，root 或有 sudo 的账号）：
#    sudo bash setup.sh ice.example.com
#  前提：域名 A 记录已指向本机公网 IP
# ============================================================
set -euo pipefail

DOMAIN="${1:-}"
if [ -z "$DOMAIN" ]; then
  echo "用法: sudo bash setup.sh <你的域名>"
  exit 1
fi

echo "==> 1/7 安装 Node.js 18 与 Nginx"
apt-get update -y
apt-get install -y curl ca-certificates nginx certbot python3-certbot-nginx ufw
if ! command -v node >/dev/null || [ "$(node -v | cut -d. -f1 | tr -d v)" -lt 18 ]; then
  curl -fsSL https://deb.nodesource.com/setup_18.x | bash -
  apt-get install -y nodejs
fi
node -v

echo "==> 2/7 创建运行用户与目录"
id -u icebot >/dev/null 2>&1 || useradd --system --home /opt/icebot --shell /usr/sbin/nologin icebot
mkdir -p /opt/icebot /var/lib/icebot
# 假设代码已上传到 /opt/icebot/server
if [ ! -f /opt/icebot/server/server.js ]; then
  echo "!! 请先把 server/ 目录上传到 /opt/icebot/server/ 再运行本脚本"
  exit 1
fi
chown -R icebot:icebot /opt/icebot /var/lib/icebot

echo "==> 3/7 生成随机令牌"
DEVICE_TOKEN="$(openssl rand -hex 24)"
APP_TOKEN="$(openssl rand -hex 24)"
cat > /etc/icebot.env <<EOF
PORT=8080
DEVICE_TOKEN=${DEVICE_TOKEN}
APP_TOKEN=${APP_TOKEN}
DATA_FILE=/var/lib/icebot/state.json
EOF
chmod 600 /etc/icebot.env

echo "==> 4/7 安装 systemd 服务"
cat > /etc/systemd/system/icebot-server.service <<EOF
[Unit]
Description=IceBot cloud relay
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=icebot
Group=icebot
WorkingDirectory=/opt/icebot/server
EnvironmentFile=/etc/icebot.env
ExecStart=/usr/bin/node /opt/icebot/server/server.js
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true
ReadWritePaths=/var/lib/icebot

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now icebot-server
sleep 1
systemctl --no-pager --lines=5 status icebot-server || true

echo "==> 5/7 配置 Nginx 反向代理"
cat > /etc/nginx/sites-available/icebot <<EOF
limit_req_zone \$binary_remote_addr zone=icebot:10m rate=10r/s;

server {
    listen 80;
    server_name ${DOMAIN};
    location /.well-known/acme-challenge/ { root /var/www/html; }
    location / { return 301 https://\$host\$request_uri; }
}
EOF
ln -sf /etc/nginx/sites-available/icebot /etc/nginx/sites-enabled/icebot
nginx -t && systemctl reload nginx

echo "==> 6/7 申请 HTTPS 证书"
certbot --nginx -d "${DOMAIN}" --non-interactive --agree-tos -m "admin@${DOMAIN}" --redirect || {
  echo "!! 证书申请失败，检查域名解析是否已生效"; exit 1; }

echo "==> 7/7 防火墙"
ufw allow 22/tcp
ufw allow 80/tcp
ufw allow 443/tcp
ufw --force enable || true

echo
echo "============================================================"
echo " 部署完成"
echo " 设备轮询地址 : https://${DOMAIN}/device/sync"
echo " 设备令牌     : ${DEVICE_TOKEN}"
echo " 小程序令牌   : ${APP_TOKEN}"
echo
echo " 设备串口执行："
echo "   cloud https://${DOMAIN}/device/sync ${DEVICE_TOKEN}"
echo "   cloud poll 10"
echo
echo " 小程序：设备地址填 https://${DOMAIN} ，令牌填 ${APP_TOKEN}"
echo " 微信后台要把 ${DOMAIN} 加入 request 合法域名"
echo "============================================================"
