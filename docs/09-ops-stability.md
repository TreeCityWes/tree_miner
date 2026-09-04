# Mining stability troubleshooting

Separate miner-process failures from whole-machine failures before changing
software or hardware. A process segfault and a host reset can have different
causes even when both interrupt mining.

## Process exits

For a systemd installation, inspect the service and its recent journal:

```bash
systemctl status treeminer.service
journalctl -u treeminer.service --since '1 hour ago'
coredumpctl info xenblocksMiner
```

Use log display mode under a service (`--display logs`). Preserve the resolved
journal path and the launch arguments when reproducing an issue. A RelWithDebInfo
build can improve stack traces; keep performance comparisons on matched builds.

If durability failure is reported, inspect the disk, journal permissions, and
SQLite error before restarting repeatedly. Keep the journal and fallback files
available for recovery. See the [miner design](../treeminer/PLAN.md) for the
journal-first submission contract.

## Whole-machine resets

Inspect kernel logs and hardware error reports. Machine-check events may identify
CPU, memory-controller, or memory faults that a miner-process stack trace cannot
explain. Compare driver, kernel, firmware, and hardware changes independently;
changing several at once makes attribution difficult.

A useful investigation records the event timestamp, software versions, GPU
utilization, temperature, power settings, and whether other GPU workloads were
active. Keep raw host identifiers, addresses, and crash artifacts local; publish
only the details needed to reproduce the issue.

## GPU correctness and performance

Allow startup CPU/GPU self-tests to validate the actual compiled path. Historical
CUDA toolchains have produced incorrect GPU first-block results, so toolchain
age alone is not sufficient evidence to enable that path. Consult the current
[build instructions](../treeminer/doc/BUILD_INSTRUCTIONS.md).

For tuning, compare one parameter at a time or use a documented configuration
matrix, validate digests, and retain a known-good launch configuration. Avoid
concurrent benchmark and production workloads on the same GPU. See the
[CUDA tuning guide](../treeminer/docs/CUDA_TUNING.md).

## Historical context

Earlier development encountered both process crashes and host resets. That
operator incident diary has been replaced by this general guide. Its old
machine-specific launch commands and temporary restrictions are not current
installation instructions. Architecture decisions remain in the miner design;
release changes are recorded in the [changelog](../CHANGELOG.md).
