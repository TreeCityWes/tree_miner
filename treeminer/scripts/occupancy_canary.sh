#!/usr/bin/env bash
# GPU-box occupancy canary. Does not open the live find journal.
# Default mining path stays warps_per_block=1; this script only measures.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-${XENBLOCKS_MINER:-$ROOT/../build/bin/xenblocksMiner}}"
GOLDEN="${2:-${TREEMINER_WARPS_GOLDEN:-$(dirname "$BIN")/treeminer_warps_golden}}"
OUT_DIR="${OCCUPANCY_OUT:-$ROOT/../.benchmarks/occupancy}"
SALT="${HASH_API_SALT:-aabbccddeeff0011}"
DEVICE="${CUDA_DEVICE:-0}"
DIFFICULTY="${OCCUPANCY_DIFFICULTY:-4096}"
BATCH="${OCCUPANCY_BATCH:-512}"
SECONDS="${OCCUPANCY_SECONDS:-4}"

mkdir -p "$OUT_DIR"

if [[ ! -x "$BIN" ]]; then
  echo "miner binary not found: $BIN" >&2
  echo "pass the xenblocksMiner path as argv1 or set XENBLOCKS_MINER" >&2
  exit 1
fi

if [[ -x "$GOLDEN" ]]; then
  echo "== warps-per-block golden (8 jobs, host first-blocks, warps=1 vs 4) =="
  "$GOLDEN" 4 "$DEVICE" | tee "$OUT_DIR/golden-warps4.txt"
  echo "== warps-per-block golden (warps=2 and 8) =="
  "$GOLDEN" 2 "$DEVICE" | tee "$OUT_DIR/golden-warps2.txt"
  "$GOLDEN" 8 "$DEVICE" | tee "$OUT_DIR/golden-warps8.txt"
else
  echo "SKIP golden binary not found: $GOLDEN"
fi

echo "== hash-benchmark A/B (warps 1 vs 2 vs 4 vs 8), no journal =="
for warps in 1 2 4 8; do
  echo "-- warps_per_block=$warps --"
  "$BIN" hash-benchmark \
    --backend cuda \
    --device "$DEVICE" \
    --salt "$SALT" \
    --difficulty "$DIFFICULTY" \
    --batch-size "$BATCH" \
    --seconds "$SECONDS" \
    --gpu-first-blocks \
    --warps-per-block "$warps" \
    --no-xuni \
    --json \
    | tee "$OUT_DIR/bench-warps${warps}.json"
done

if command -v ncu >/dev/null 2>&1; then
  echo "== Nsight Compute occupancy (oneshot kernel) =="
  for warps in 1 4; do
    ncu \
      --target-processes all \
      --kernel-name regex:argon2_kernel_oneshot \
      --launch-count 3 \
      --section Occupancy \
      --log-file "$OUT_DIR/ncu-warps${warps}.log" \
      "$BIN" hash-batch \
        --backend cuda \
        --device "$DEVICE" \
        --salt "$SALT" \
        --difficulty "$DIFFICULTY" \
        --batch-size "$BATCH" \
        --gpu-first-blocks \
        --warps-per-block "$warps" \
        --no-xuni \
        --json \
      > "$OUT_DIR/ncu-warps${warps}-hash.json" || {
        echo "ncu failed for warps=$warps (permissions or metric set). See $OUT_DIR/ncu-warps${warps}.log" >&2
      }
  done
else
  echo "SKIP ncu not on PATH. Install CUDA Nsight Compute on the GPU box for occupancy counters."
fi

echo "wrote $OUT_DIR"
