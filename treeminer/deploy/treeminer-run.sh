#!/bin/bash
# Single miner invocation. systemd Restart=always (or boot-ensure.sh) restarts us.
set -euo pipefail
cd /home/wes/tree_miner/runtime-live
export NO_COLOR=1
export CUDA_DEVICE_ORDER=PCI_BUS_ID
export TREEMINER_NO_TUI=1
ulimit -c unlimited || true
for i in $(seq 1 30); do
  if nvidia-smi >/dev/null 2>&1; then
    break
  fi
  sleep 2
done
exec /home/wes/tree_miner/build-sm86-cuda13/bin/xenblocksMiner \
  --execute \
  --minerAddr 0xe4bB184781bBC9C7004e8DafD4A9B49d203BC9bC \
  --totalDevFee 0 \
  --display logs \
  --dashboard-bind 0.0.0.0 \
  --dashboard-port 42069 \
  --journalPath /home/wes/tree_miner/runtime-live/treeminer-journal.db \
  --cpuWorkers 0 \
  --cudaStreams 2
