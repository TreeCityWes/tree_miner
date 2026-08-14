#!/bin/bash
# Stock-safe GPU state at boot. No overclock.
# nvidia-persistenced on this Ubuntu package is started with --no-persistence-mode,
# so Persistence Mode stays Off unless we flip it here after the daemon is up.
set -euo pipefail
for i in $(seq 1 15); do
  if /usr/bin/nvidia-smi >/dev/null 2>&1; then
    break
  fi
  sleep 2
done
/usr/bin/nvidia-smi -pm 1
for i in 0 1; do
  /usr/bin/nvidia-smi -i "$i" -pl 170 || true
done
