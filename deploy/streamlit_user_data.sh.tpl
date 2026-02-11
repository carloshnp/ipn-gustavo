#!/bin/bash
set -euo pipefail

APP_DIR="/opt/chamber-dashboard"
VENV_DIR="$APP_DIR/.venv"
ENV_FILE="/etc/chamber-dashboard.env"
SERVICE_FILE="/etc/systemd/system/chamber-streamlit.service"

mkdir -p "$APP_DIR"

dnf makecache -y
dnf install -y python3 python3-pip

cat > "$ENV_FILE" <<'ENVEOF'
AWS_REGION=__AWS_REGION__
TS_DB=__TS_DB__
TS_TABLE_TELEMETRY=__TS_TABLE_TELEMETRY__
TS_TABLE_EVENTS=__TS_TABLE_EVENTS__
APP_USER=__APP_USER__
APP_PASSWORD_SHA256=__APP_PASSWORD_SHA256__
ENVEOF

cat <<'APPB64' | base64 -d > "$APP_DIR/streamlit_app.py"
__APP_B64__
APPB64

cat <<'REQB64' | base64 -d > "$APP_DIR/requirements.txt"
__REQ_B64__
REQB64

python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$APP_DIR/requirements.txt"

cat > "$SERVICE_FILE" <<'SERVICEEOF'
[Unit]
Description=Chamber Streamlit Dashboard
After=network.target

[Service]
Type=simple
User=ec2-user
WorkingDirectory=/opt/chamber-dashboard
EnvironmentFile=/etc/chamber-dashboard.env
ExecStart=/opt/chamber-dashboard/.venv/bin/streamlit run /opt/chamber-dashboard/streamlit_app.py --server.port 8501 --server.address 0.0.0.0
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SERVICEEOF

chown -R ec2-user:ec2-user "$APP_DIR"
chmod 600 "$ENV_FILE"

systemctl daemon-reload
systemctl enable chamber-streamlit.service
systemctl restart chamber-streamlit.service
