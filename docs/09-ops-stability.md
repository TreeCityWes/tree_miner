# Ops stability — this box (treehome01)

**Date:** 2026-08-13
**Host:** `treehome01` — Xeon E5-2620 (2012), 62 GiB RAM, 2× RTX 3060 (sm_86)
**Follow up:** later this week. Do not change the running miner unless it dies.

This is the incident record from 2026-08-10–13. It is not a design authority (`treeminer/PLAN.md` still owns the journal/submit contract).

## What is running now

| Unit | Role |
|---|---|
| `treeminer.service` | TreeMiner, `Restart=always`, enabled on boot, `--display logs` only |
| `gpu-tune.service` | `nvidia-smi -pm 1` + 170 W cap after `nvidia-persistenced` |
| `rasdaemon.service` | Decodes machine-check exceptions |

- Binary: `/home/wes/tree_miner/build-sm86-gcc10/bin/xenblocksMiner`
- Journal: `/home/wes/tree_miner/runtime-live/treeminer-journal.db` (`--journalPath` is absolute)
- Dashboard: `http://192.168.1.50:42069` (`TREEMINER_NO_TUI=1`, TUI refused under systemd)
- Cores: `systemd-coredump` (`coredumpctl gdb xenblocksMiner`); `ExecStopPost` snapshots unexpected exits to `runtime-live/crash-snapshots/`
- **Off on purpose:** `comfyui`, `ollama`, `emojidracbot` (the bot `Requires=` the other two, so it must stay disabled or they come back)
- Install / repair: `treeminer/deploy/install-treeminer-service.sh`, `setup-hardware-guards.sh`

`xenblocks-pub-miner.service` (Woody / xnm.pub) is disabled. Do not start it alongside TreeMiner.

## Two different failures

Keep these separate.

1. **Process segfault** (`xenblocksMiner[...] segfault` in `dmesg`). TreeMiner software. Two logged:
   - 2026-08-12 01:05 — jump to address 0
   - 2026-08-13 04:49 — read address 2 inside libc, `--display terminal`
2. **Whole-box hard reset** (`last` shows `crash`, kernel `mce: [Hardware Error]`). CPU/memory-controller signal. Not a C++ exception.

Crash-hardening for (1) is in the live binary (uncommitted as of 2026-08-13): TUI/stdout one lock, SIGINT only sets `running`, dashboard thread joined, `localtime_r`. See `treeminer/CHANGES-FROM-UPSTREAM.md`.

## Timeline (do not collapse this)

Woody + this box ran **45 days straight** (2026-06-27 → 2026-08-10) on NVIDIA **595.71** and kernel 5.15.0-185. Nine MCEs in 45 days. No reset storm.

| When | GPUs | Other change | Host |
|---|---|---|---|
| 2026-06-27 – 08-10 | Woody / xnm.pub | 595.71 + kernel 185 | 45 d up, 9 MCEs |
| **2026-08-10 22:28** | **Woody** (pub-miner started 22:28:33) | TreeMiner repo existed; not on the cards | First crash after 45 d |
| 2026-08-11 06:08 | Woody | **595.71 → 595.84** and **kernel 187** same apt | — |
| 2026-08-12 01:28 – 22:11 | Woody | 595.84 | Crashed 22:11 |
| 2026-08-12 23:44 | Switched to TreeMiner | pub-miner `disable --now` | — |
| 2026-08-13 01:32, 08:02 | TreeMiner | 08:02 died mid `apt-get` | Two more hard resets |
| 2026-08-13 01:32 – 08:01 | TreeMiner | — | 44 MCEs in 6.5 h |

TreeMiner *does* work the host harder than Woody (SQLite `synchronous=FULL` fsync per find, submit thread, Crow dashboard). That can knock over marginal RAM/PSU/PCIe. It cannot *write* an MCE. The first reset and the 22:11 reset were still Woody. The MCE rate jumped after **595.84 + kernel 187**.

## Follow up later this week

In this order. One experiment at a time. Leave the miner alone until you start one.

1. **Decode the next MCE** (no reboot):
   ```bash
   sudo ras-mc-ctl --errors
   sudo ras-mc-ctl --summary
   journalctl -k -b -g 'mce: \[Hardware Error\]'
   ```
   If it names a DIMM, it is not the miner.
2. **A/B: Woody vs TreeMiner on this same 595.84 / kernel 187.** Put Woody back for 12–24 h. If it still resets, the refactor is not the host-killer. If it stays up, TreeMiner’s host path is the stressor. Only one miner at a time.
3. **A/B: roll NVIDIA back to 595.71** (the driver that survived 45 days) while staying on TreeMiner. If resets stop, 595.84 is the suspect.
4. **Memtest** when the cards can be idle overnight: `sudo apt install memtest86+`, reboot into the GRUB memtest entry, several passes.
5. After a *process* segfault (not a host reset):
   ```bash
   /home/wes/tree_miner/treeminer/deploy/diagnose-last-crash.sh
   coredumpctl gdb xenblocksMiner
   ```
   If frames are `??`, rebuild RelWithDebInfo once.

Do not enable GPU first-blocks. Startup still reports a CPU/CUDA digest mismatch; that path 401s on the real server.

## Do not do

- Start `emojidracbot` / `comfyui` / `ollama` on this box while mining.
- Run `--display terminal` under systemd (refused even if the flag is edited).
- Blame parked journal rows at `m=100`/`1100` while server difficulty is 2100 — that is `ParkedDifficulty` working.

## Commands

```bash
systemctl status treeminer.service gpu-tune.service rasdaemon.service
nvidia-smi --query-gpu=index,persistence_mode,utilization.gpu --format=csv
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
tail -n 40 /home/wes/tree_miner/runtime-live/miner-service.log
```
