#!/bin/bash
# systemd ExecStopPost. Snapshot unexpected miner deaths so the next
# segfault is not just one kernel line.
set -u
result="${SERVICE_RESULT:-}"
code="${EXIT_CODE:-}"
status="${EXIT_STATUS:-}"
if [[ "$result" == "success" ]]; then
  exit 0
fi
dir=/home/wes/tree_miner/runtime-live/crash-snapshots
mkdir -p "$dir"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
out="$dir/$stamp-$result-$code-$status.txt"
{
  echo "time=$(date -Is)"
  echo "SERVICE_RESULT=$result EXIT_CODE=$code EXIT_STATUS=$status"
  echo
  echo '=== dmesg (xenblocks / segfault / NVRM / Xid / mce) ==='
  dmesg -T | tail -n 200 | grep -Ei 'xenblocks|segfault|NVRM|Xid|mce|oom' || true
  echo
  echo '=== nvidia-smi ==='
  nvidia-smi || true
  echo
  echo '=== last miner-service.log ==='
  tail -n 80 /home/wes/tree_miner/runtime-live/miner-service.log || true
} >"$out" 2>&1
echo "wrote $out"
exit 0
