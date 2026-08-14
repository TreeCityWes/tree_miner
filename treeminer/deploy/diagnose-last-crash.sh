#!/bin/bash
# Read-only: what we know about the latest miner death.
set -euo pipefail
echo '=== treeminer.service ==='
systemctl --no-pager --full status treeminer.service | head -25 || true
echo
echo '=== process crashes (this boot + previous) ==='
journalctl -k --no-pager -g 'xenblocksMiner|segfault' --since '7 days ago' | tail -n 30 || true
echo
echo '=== service stops / restarts (this boot) ==='
journalctl -u treeminer.service --no-pager -o short-iso | tail -n 40 || true
echo
echo '=== cores ==='
ls -lt /home/wes/tree_miner/runtime-live/cores 2>/dev/null | head || echo 'no local cores dir yet'
if command -v coredumpctl >/dev/null 2>&1; then
  coredumpctl list xenblocksMiner 2>/dev/null | tail -n 10 || echo 'no systemd-coredump entries'
fi
echo
echo '=== crash snapshots ==='
ls -lt /home/wes/tree_miner/runtime-live/crash-snapshots 2>/dev/null | head || echo 'none yet'
echo
echo '=== GPU processes now ==='
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv || true
