#!/bin/bash
# Stop ComfyUI / Ollama so they cannot steal the 3060s, and make the next
# TreeMiner segfault leave a core we can actually open in gdb.
# Requires sudo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

echo '=== stopping GPU competitors ==='
sudo systemctl stop comfyui.service ollama.service 2>/dev/null || true
sudo systemctl disable comfyui.service ollama.service 2>/dev/null || true

echo '=== installing crash dumps ==='
sudo mkdir -p /home/wes/tree_miner/runtime-live/cores
sudo chown wes:wes /home/wes/tree_miner/runtime-live/cores
if sudo apt-get install -y systemd-coredump; then
  echo 'systemd-coredump installed (coredumpctl gdb xenblocksMiner after a crash)'
else
  echo 'kernel.core_pattern=/home/wes/tree_miner/runtime-live/cores/core.%e.%p.%s.%t' | \
    sudo tee /etc/sysctl.d/99-treeminer-cores.conf >/dev/null
  sudo sysctl -p /etc/sysctl.d/99-treeminer-cores.conf
fi

echo '=== refreshing treeminer unit (ExecStopPost snapshot; no miner restart) ==='
sudo cp "$ROOT/treeminer.service" /etc/systemd/system/treeminer.service
sudo systemctl daemon-reload

echo
echo '=== leftover GPU compute ==='
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
echo
echo '=== services ==='
systemctl is-active comfyui.service ollama.service treeminer.service
echo
echo '=== core_pattern ==='
cat /proc/sys/kernel/core_pattern
