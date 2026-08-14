#!/bin/bash
# Installs treeminer.service and enables it on boot. Requires sudo.
set -euo pipefail
UNIT_SRC="$(cd "$(dirname "$0")" && pwd)/treeminer.service"
if [[ ! -f "$UNIT_SRC" ]]; then
  echo "missing $UNIT_SRC" >&2
  exit 1
fi

DEPLOY="$(cd "$(dirname "$0")" && pwd)"
sudo chmod +x "$DEPLOY/gpu-boot-tune.sh"
sudo cp "$DEPLOY/gpu-tune.service" /etc/systemd/system/gpu-tune.service
sudo cp "$UNIT_SRC" /etc/systemd/system/treeminer.service
sudo systemctl daemon-reload
sudo systemctl enable gpu-tune.service
sudo systemctl disable --now xenblocks-pub-miner.service xenblocks-miner.service xenblocksminer.service 2>/dev/null || true
sudo systemctl disable --now comfyui.service ollama.service 2>/dev/null || true

# Stop the user-level supervise fallback if it is running.
if [[ -f /home/wes/tree_miner/runtime-live/treeminer-supervise.pid ]]; then
  pid="$(cat /home/wes/tree_miner/runtime-live/treeminer-supervise.pid || true)"
  if [[ -n "${pid}" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    sleep 1
  fi
fi
pkill -x xenblocksMiner 2>/dev/null || true
sleep 1

sudo systemctl enable --now treeminer.service
systemctl is-enabled treeminer.service
systemctl --no-pager --full status treeminer.service || true
