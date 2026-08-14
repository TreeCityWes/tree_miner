#!/bin/bash
# Boot fallback when the system unit is not enabled (needs root to install).
# No-ops once `treeminer.service` is enabled so we cannot start two miners.
set -euo pipefail
if systemctl is-enabled treeminer.service >/dev/null 2>&1; then
  exit 0
fi
mkdir -p /home/wes/tree_miner/runtime-live
exec 9>/home/wes/tree_miner/runtime-live/treeminer-supervise.lock
if ! flock -n 9; then
  exit 0
fi
echo "$$" > /home/wes/tree_miner/runtime-live/treeminer-supervise.pid
while true; do
  /home/wes/tree_miner/treeminer/deploy/treeminer-run.sh >>/home/wes/tree_miner/runtime-live/miner-service.log 2>&1 || true
  echo "$(date -Is) treeminer exited; restarting in 15s" >>/home/wes/tree_miner/runtime-live/miner-service.log
  sleep 15
done
