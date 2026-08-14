#!/bin/bash
# 1) Decode machine-check errors (rasdaemon)
# 2) Enable NVIDIA persistence now and on every boot
# Does not restart the miner.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

echo '=== stop GPU competitors (emojidracbot Requires= comfyui+ollama, so disable is not enough) ==='
sudo systemctl disable --now emojidracbot.service comfyui.service ollama.service 2>/dev/null || true

echo '=== rasdaemon ==='
if ! dpkg -s rasdaemon >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y rasdaemon
fi
sudo systemctl enable --now rasdaemon.service

echo '=== GPU persistence (now + boot) ==='
sudo cp "$ROOT/gpu-tune.service" /etc/systemd/system/gpu-tune.service
sudo chmod +x "$ROOT/gpu-boot-tune.sh"
sudo systemctl daemon-reload
sudo systemctl enable --now gpu-tune.service

# Refresh miner unit so it waits for gpu-tune after a reboot. No restart.
sudo cp "$ROOT/treeminer.service" /etc/systemd/system/treeminer.service
sudo systemctl daemon-reload

echo
echo '=== rasdaemon ==='
systemctl is-enabled rasdaemon.service
systemctl is-active rasdaemon.service
echo
echo '=== gpu-tune ==='
systemctl is-enabled gpu-tune.service
systemctl is-active gpu-tune.service
echo
echo '=== persistence ==='
nvidia-smi --query-gpu=index,persistence_mode,power.limit --format=csv
echo
echo '=== miner (should still be running) ==='
systemctl is-active treeminer.service
pgrep -a -x xenblocksMiner || true
echo
echo '=== recent MCE decode (may be empty until the next event) ==='
sudo ras-mc-ctl --errors 2>/dev/null | tail -n 40 || true
sudo ras-mc-ctl --summary 2>/dev/null | tail -n 40 || true
