# 03 — xgpu (Jozef Miner) — Technical Analysis

Source: `C:\projects\treeminer_xnm\repos\xgpu` (GitHub: `JozefJarosciak/xgpu`), read in full line by line.
Supplementary source: `JozefJarosciak/XENGPUMiner` (see "External miner code" caveat in §1) — fetched live via GitHub raw/API on 2026-08-09 because the original upstream repo it clones (`shanhaicoder/XENGPUMiner`) now returns `404 Not Found` on GitHub (repo deleted or renamed).

---

## 1. What this project actually is

`xgpu` is **not a miner**. It contains **zero mining logic, zero hashing code, and zero networking code**. The entire repository is:

- `readme.md` — a tutorial (fork-this-repo, edit-your-wallet, rent-a-GPU-box, paste-a-one-liner) aimed at Vast.ai / RunPod renters.
- Seven shell scripts (`vast.sh`, `vast4.sh`, `vast8.sh`, `vast12.sh`, `vast14.sh`, `vast-autogpu.sh`, `runpod.sh`) that are **bootstrap/deployment wrappers**. Each one, on a fresh cloud GPU box:
  1. installs OS packages,
  2. `git clone`s an entirely separate, external miner project — **`shanhaicoder/XENGPUMiner`** (a Python orchestrator + a compiled CUDA/OpenCL binary called `xengpuminer`),
  3. patches that external project's `config.conf` with a hard-coded wallet address via `sed`,
  4. launches the Python process and one `xengpuminer` process per GPU with `nohup`, then tails a log file forever.

So "xgpu" is best understood as **cloud-deployment glue for someone else's miner**. The real mining engine (hashing, difficulty handling, server submission, retry logic) lives in the external `XENGPUMiner` repo, which these scripts `git clone` fresh on every run — the xgpu repo has no vendored copy and does not pin a commit/tag, so behavior can change silently upstream at any time.

**Important finding for our project**: the upstream repo `shanhaicoder/XENGPUMiner` that all seven scripts clone (`repos/xgpu/vast.sh:23` etc.) is **currently gone from GitHub** (`https://github.com/shanhaicoder/XENGPUMiner` → 404 as of 2026-08-09; `gh api repos/shanhaicoder/XENGPUMiner/contents/` also 404). Every one-liner in the readme (`repos/xgpu/readme.md:65-87`) and every `git clone` in the scripts is now a **broken/dead dependency** unless GitHub's cache or a fork still serves it. The author's own fork, `JozefJarosciak/XENGPUMiner`, is still live and appears to be the same lineage (identical `config.conf` default wallet address, identical `sed` target string used in xgpu's scripts), so it was used below as the best available stand-in to document the actual miner internals — but it is main-branch HEAD today, not necessarily byte-identical to whatever `shanhaicoder/XENGPUMiner` served historically when xgpu's scripts were written (Oct–Nov 2023 per git log).

---

## 2. Per-script breakdown

All scripts share one skeleton: `apt` install → `git clone` external miner → `build.sh` → `sed` wallet patch → `pip install` → launch Python + N× GPU binary → `tail -f`. They differ only in GPU count and how much output is suppressed.

### `vast.sh` — 1 GPU (`repos/xgpu/vast.sh`)
- Line 8: `sudo apt -y upgrade` (no `apt update` in current version — an earlier commit removed it, see git log `2d21170`).
- Lines 11-20: installs `ocl-icd-opencl-dev` (OpenCL dev headers), `nano`, `cmake`, `python3-pip`. All output redirected to `/dev/null` (silent installs).
- Line 23: `sudo git clone https://github.com/shanhaicoder/XENGPUMiner.git`.
- Lines 26-28: `cd XENGPUMiner`, `chmod +x build.sh`, `./build.sh` — compiles the CUDA/OpenCL miner binary via CMake.
- Line 31: `sudo sed -i 's/account = 0x24691e.../account = 0xca5F023.../g' config.conf` — see §2 wallet-handling note below.
- Line 34: `sudo pip install -U -r requirements.txt`.
- Lines 38-39: launches **two** background processes:
  - `sudo nohup python3 miner.py --gpu=true > miner.log 2>&1 &` — the Python orchestrator (difficulty polling, submission, XUNI queue).
  - `sudo nohup ./xengpuminer > xengpuminer.log 2>&1 &` — the single-GPU compiled hash-finder binary.
- Line 46: `tail -f /root/XENGPUMiner/miner.log` keeps the SSH session attached to the Python log forever (this is also what keeps the cloud instance's foreground process alive on Vast.ai/RunPod, which typically kill the container when the entrypoint process exits).
- No env vars, no CLI flags beyond `--gpu=true`. Wallet is baked into `config.conf`, not passed via env var or argument.

### `vast4.sh` — 4 GPUs (`repos/xgpu/vast4.sh`)
Identical to `vast.sh` through the `pip install` step. Difference is the launch block (lines 38-46): starts `python3 miner.py` once, then loops `./xengpuminer -d0` through `-d3` (one process per GPU index, `-d` = device index), each with `sleep 1` between launches (crude stagger to avoid simultaneous CUDA context init) and its own log file `xengpuminer-N.log`.

### `vast8.sh` — 8 GPUs (`repos/xgpu/vast8.sh`)
Same pattern, extended to `-d0` .. `-d7` (lines 40-54), 8 separate log files.

### `vast12.sh` — 12 GPUs (`repos/xgpu/vast12.sh`)
Same pattern, `-d0` .. `-d11` (lines 40-62).

### `vast14.sh` — 14 GPUs (`repos/xgpu/vast14.sh`)
Same pattern, `-d0` .. `-d13` (lines 40-66). This is the largest fixed-count script; note the readme (`readme.md:79-87`) mislabels the 12- and 14-GPU download commands as "use vast8.sh" — a documentation bug (the URLs are correct, only the human-readable label is wrong).

### `vast-autogpu.sh` — auto-detected GPU count (`repos/xgpu/vast-autogpu.sh`)
The only script that doesn't hard-code a GPU count. Differences from the others:
- Not silenced (`apt` output not redirected — line 4-9 uses plain `apt update`/`apt -y install ...` with visible output).
- Additionally installs `htop` and `nvtop` (lines 7-8) for interactive monitoring — the only script that installs monitoring tools.
- Line 43: `num_gpus=$(lspci | grep -i nvidia | wc -l)` — detects GPU count by counting NVIDIA PCI devices rather than requiring a matching script per GPU count.
- Lines 46-50: `for gpu_id in $(seq 0 $((num_gpus - 1))); do sudo nohup ./xengpuminer -d$gpu_id > xengpuminer-$gpu_id.log 2>&1 & sleep 1; done` — dynamically spawns one miner process per detected GPU. This is strictly better than the fixed vast4/8/12/14 scripts and is the pattern worth reusing.
- Note: `lspci | grep -i nvidia` counts *all* NVIDIA PCI functions (including e.g. audio controllers on some cards), so it can over-count on certain hardware — a fragile heuristic; `nvidia-smi -L | wc -l` would be more accurate.

### `runpod.sh` — RunPod variant (`repos/xgpu/runpod.sh`)
Structurally the odd one out:
- No `sudo` anywhere (RunPod containers commonly already run as root) — lines 4-22.
- Also installs `htop` and `nvtop` (lines 7-8).
- Does **not** launch `xengpuminer` (the compiled GPU binary) directly, or spawn per-GPU processes at all. Instead, line 23 runs `nohup ./miner.sh > miner.log 2>&1 &` — delegating entirely to a `miner.sh` script that ships **inside** the external `XENGPUMiner` clone (not present in xgpu itself), presumably a wrapper that starts both the Python process and the GPU binary(ies) itself. This means runpod.sh's actual multi-GPU behavior is opaque from the xgpu repo alone — it's whatever `miner.sh` in the external repo does at clone time.
- No `tail -f` at the end, so the script returns immediately after backgrounding the process (relies on the container's own supervisor to stay alive).

### Wallet / account handling (all scripts)
Every script applies the exact same `sed` substitution (e.g. `repos/xgpu/vast.sh:31`, `repos/xgpu/runpod.sh:19`):
```
sed -i 's/account = 0x24691e54afafe2416a8252097c9ca67557271475/account = 0xca5F023af4F822353A563Ae6a3591bA2024495E8/g' config.conf
```
- `0x24691e54afafe2416a8252097c9ca67557271475` is the **external repo's own default/placeholder address** shipped in its `config.conf` (confirmed live at `JozefJarosciak/XENGPUMiner/config.conf`, line 4).
- `0xca5F023af4F822353A563Ae6a3591bA2024495E8` is **Jozef's own address**, baked into the upstream public template that everyone downloads.
- The readme's entire workflow (`readme.md:6-28`) exists *because* of this: users are instructed to **fork the repo and hand-edit this literal string in each script** before running the one-liner. If a user skips that step and just runs the published one-liners directly against `JozefJarosciak/xgpu/main/...`, all mining rewards route to the author's wallet, not theirs. There is no CLI flag, env var, or prompt for wallet address anywhere in these scripts — it is 100% "edit source, commit fork, then wget your fork's raw URL." This is a meaningful operational fragility to avoid in a new miner (wallet should be a runtime config/env var, never a value baked into a script that has to be forked to change).
- Separately, the external `miner.py` has its own **developer-fee mechanism**, unrelated to the sed patch: during the first minute of every hour, if `dev_fee_on=true`, submissions are attributed to the *original* placeholder address (`0x24691e...`) instead of the configured account (fork `miner.py` lines 487-494). `config.conf`'s `dev_fee_on` defaults to `false` (`JozefJarosciak/XENGPUMiner/config.conf` line ~12), and none of xgpu's scripts touch this setting, so it stays off by default in this deployment path.

---

## 3. Network endpoints, difficulty source, and submission logic

None of this logic lives in the `xgpu` repo — it all lives in the external, cloned `XENGPUMiner` project, driven by `config.conf` and `miner.py`. Documented here from the live fork (`JozefJarosciak/XENGPUMiner`) since that's the closest available copy to what these scripts fetch:

**`config.conf`** (defaults shipped in the template, patched only for `account` by xgpu's `sed`):
```
difficulty = 1
memory_cost = 1500
cores = 1
account = 0x24691e54afafe2416a8252097c9ca67557271475
last_block_url = http://xenminer.mooo.com:4445/getblocks/lastblock
gpu_mode = true
dev_fee_on = false
```

**Endpoints used by `miner.py`** (all plain HTTP, no TLS, no API key/auth):
| Endpoint | Purpose | Reference |
|---|---|---|
| `http://xenminer.mooo.com/difficulty` | GET — poll current `memory_cost` (Argon2 memory-hardness parameter used as difficulty) | `miner.py` `fetch_difficulty_from_server()`, ~line 251 |
| `http://xenminer.mooo.com/verify` | POST — submit a found hash (`hash_to_verify`, `key`, `account`, `attempts`, `hashes_per_second`, `worker`) for validation/credit | `miner.py` `submit_block()` line ~518, `mine_block()` line ~425, `submit_xuni_records()` line ~30 |
| `http://xenminer.mooo.com:4445/getblocks/lastblock` | GET — fetch the last sealed block's records (`config.conf: last_block_url`) needed to build a Merkle root for the following proof-of-work submission | `submit_pow()` line ~271 |
| `http://xenminer.mooo.com:4446/send_pow` | POST — submit proof-of-work (`account_address`, `block_id`, `merkle_root`, `key`, `hash_to_verify`) after verifying a batch of the previous block's hashes | `submit_pow()` line ~322 |

**Difficulty**: not fixed — polled from the server (`/difficulty`) on a background thread (`update_memory_cost_periodically`, driving `write_difficulty_to_file()` → `difficulty.txt`) and used to parameterize the Argon2 hasher (`memory_cost` parameter). The GPU binary (`xengpuminer`, compiled C++/CUDA, source not in this repo) presumably reads the same `difficulty.txt`/`config.conf` to size its search; the exact interface between the Python process and the compiled binary is not visible from xgpu or from the Python source alone (the binary writes candidate hashes to a `gpu_found_blocks_tmp/` directory that `monitor_blocks_directory()` polls).

**Submission/retry logic** (from `miner.py`, all HTTP, no queueing framework, no persistence layer):
- Normal/superblock (`XEN11`) hashes: POST to `/verify` with up to `max_retries = 5` attempts, sleeping 3s between retries, but **only retries on HTTP 500**; any other non-200 status (including connection errors caught by the generic `except Exception` at the bottom of the loop) falls through without a clear success/fail split, and the code path that computes the GPU-found-block payload (`monitor_blocks_directory()` → `submit_block()`) **deletes the local candidate file (`os.remove(filepath)`) unconditionally after processing**, regardless of whether submission ultimately succeeded — i.e., **a hash found while the server is unreachable is silently discarded**, not queued for later resubmission (`miner.py` `monitor_blocks_directory()`, lines ~617-626).
- CPU-mining path (`mine_block()`) has a smaller retry budget (`max_retries = 2`, 5s sleep) and the same 500-only retry gate.
- `XUNI` hashes (a secondary/bonus block type only valid within a 5-minute window at the top of each hour, gated by `is_within_five_minutes_of_hour()`) are the **one place with any offline-resilience attempt**: `add_xuni_record()` appends the payload to an **in-memory Python list** (`xuni_records`), and a background thread (`check_and_submit_periodically()`) flushes that list to `/verify` once per minute, but only while inside the 5-minute submission window, with a random 1-5s jitter between each POST. Despite a code comment claiming "Save XUNI record to disk immediately" (`miner.py` line ~381), the record is **never actually written to disk** — it lives only in a process-local list, so a crash or restart of `miner.py` loses all pending XUNI records. Successful/failed submissions are appended to append-only text logs (`XUNI/0.processed-good.txt`, `XUNI/0.processed-bad.txt`) purely for human review, not as a resumable queue.
- There is no durable local database, no SQLite, no on-disk queue, no idempotency key, and no timestamp sent with submissions beyond what the server infers from arrival time — none of the offline-resubmission behavior our project needs already exists here in a working form; the XUNI in-memory list is the closest analog and it is explicitly broken (memory-only, misleading comment, narrow time window).

---

## 4. Cloud-mining ops lessons relevant to our new miner

**What's worth borrowing:**
- **Per-GPU process isolation with per-GPU logs** (`vast4.sh` line 40 pattern: `./xengpuminer -d$i > xengpuminer-$i.log 2>&1 &`) — one OS process per device with its own log file makes it trivial to see which GPU stalled or crashed, and to kill/restart one GPU without touching the others.
- **Auto-detecting GPU count instead of hard-coding it** (`vast-autogpu.sh` lines 43-50, `num_gpus=$(lspci | grep -i nvidia | wc -l)` then a `for` loop) — this collapses what xgpu implements as five near-duplicate scripts (`vast.sh`/4/8/12/14) into one. A new miner should detect GPUs at runtime (`nvidia-smi -L` is more robust than `lspci` grep) rather than shipping N fixed launch scripts.
- **Staggered process start** (`sleep 1` between each GPU launch across vast4/8/12/14/autogpu) — avoids GPU driver/CUDA-context init contention when starting many processes at once.
- **`tail -f` as a foreground keep-alive** (`vast.sh` line 46 etc.) — simple way to keep a cloud container's entrypoint alive and give the operator a live log stream over SSH; fine for manual ops but not a substitute for a real process supervisor/health check.
- **Separating orchestration (Python) from the hot loop (compiled GPU binary)** — `miner.py` handles config, difficulty polling, and network submission while `xengpuminer` (C++/CUDA) just searches for hashes and drops results in a directory; the Python side polls that directory. This decoupling (dumb fast hash-finder + smart thin submission layer watching a spool directory) is a reasonable shape for our new miner too, and pairs naturally with "write found hash to a durable local queue first, submit asynchronously."

**What's missing or broken here that we must fix:**
- **No process supervision or auto-restart anywhere.** All processes are started once via `nohup ... &` and never restarted if they die (OOM, driver crash, unhandled exception). The readme's own "useful commands" section (`readme.md:112-137`) documents *manual* recovery only: `tail -f` to check logs, `pkill -f xengpuminer` / `pkill -f python3` to kill everything, then presumably re-run the whole install script by hand. There is no watchdog, no `systemd` unit, no `supervisord`, no restart-on-crash loop. A new miner should run under a real supervisor (systemd with `Restart=always`, or an internal watchdog thread) rather than a bare `nohup`.
- **No handling of central-server outages at all**, beyond the narrow, broken XUNI in-memory queue described in §3. Regular (non-XUNI) found hashes that fail to submit are simply dropped (`os.remove()` happens regardless of submission outcome). This is precisely the gap our project's design goal targets — persist every found hash + timestamp to durable local storage *before* attempting submission, and only clear it from the local store once the server acknowledges receipt, so a `shanhaicoder/XENGPUMiner`-style outage (or the fact that its whole GitHub repo can vanish, as happened here) doesn't lose already-found work.
- **No retry/backoff strategy beyond a fixed small retry count on HTTP 500 only.** Connection refused / timeout / DNS failure / non-500 error codes are not retried in the GPU-block path at all; the block is just lost. A new miner needs exponential backoff plus persistent retry (not "retry 5 times over 15 seconds then give up").
- **Wallet address is baked into a script that must be hand-edited and re-forked** rather than passed as a config file, env var, or CLI flag — brittle, easy to mine into the wrong wallet by accident (see §2), and painful to rotate/update across a fleet of rented machines. Use env vars/config for anything account- or endpoint-related.
- **Silent installs (`> /dev/null 2>&1`) hide failures.** Every apt/pip/build step in vast.sh/4/8/12/14 discards both stdout and stderr, so a failed `apt install` or failed CUDA `build.sh` (e.g., wrong CUDA toolkit version) produces no diagnostic — the script just proceeds to the next silently-broken step and later processes fail with a generic missing-binary error. Prefer structured logging over blanket `/dev/null` redirection.
- **External dependency with no version pinning.** Every script does a plain `git clone` of `main`/default branch with no commit SHA, tag, or checksum pin, and the referenced upstream (`shanhaicoder/XENGPUMiner`) is now a 404 — the entire fleet-deployment workflow described in the readme is currently non-functional for anyone who hasn't already forked/cached the miner code. A new miner should vendor its own code (as this project intends) rather than depend on `git clone` of a third party's default branch at instance-boot time.
- **No structured/rotated logs.** Logs are raw `nohup ... > file.log 2>&1` redirects with no rotation, size caps, or structured (JSON) format — fine for a single manual session, unsuitable for unattended fleets.
- **Plain HTTP, no TLS, no auth token** on all four server endpoints (`xenminer.mooo.com` / `:4445` / `:4446`) — submissions and difficulty queries are unauthenticated and unencrypted. Not directly xgpu's fault (that's upstream server design), but worth noting as a constraint/risk if our new miner talks to the same or a similar server.

---

## 5. Repo map

```
repos/xgpu/                      (JozefJarosciak/xgpu — deployment scripts only, no miner code)
├── readme.md                    Tutorial: fork repo → edit wallet in each vast*.sh → rent Vast.ai/RunPod GPU
│                                 box → paste one-liner wget+chmod+run command. Also documents manual
│                                 maintenance commands (tail logs, pkill, restart miner.py).
├── vast.sh                      1-GPU bootstrap: apt deps → git clone shanhaicoder/XENGPUMiner → build.sh →
│                                 sed wallet patch → pip install → launch miner.py + 1x xengpuminer → tail log
├── vast4.sh                     Same as vast.sh but launches xengpuminer -d0..-d3 (4 GPUs, fixed)
├── vast8.sh                     Same, -d0..-d7 (8 GPUs, fixed)
├── vast12.sh                    Same, -d0..-d11 (12 GPUs, fixed)
├── vast14.sh                    Same, -d0..-d13 (14 GPUs, fixed)
├── vast-autogpu.sh              Same base, but auto-detects GPU count via `lspci | grep -i nvidia` and loops;
│                                 also installs htop/nvtop; not output-silenced
└── runpod.sh                    RunPod variant: no sudo, installs htop/nvtop, delegates GPU/process
                                  orchestration to an external miner.sh (inside the cloned repo, not in xgpu)

External dependency (not part of this repo, cloned at deploy time):
  shanhaicoder/XENGPUMiner       Referenced by every script's `git clone`. Currently returns 404 on GitHub
                                  (deleted/renamed as of 2026-08-09) — see §1.
  JozefJarosciak/XENGPUMiner     Author's own fork of the same project, still live; used in this doc (§3) as
                                  the best available proxy for the external miner's actual endpoints/logic.
                                  Key files referenced: config.conf, miner.py, build.sh, requirements.txt.
```

---

## Appendix: sources and caveats

- All `repos/xgpu/*` file/line citations above are from the locally cloned repo at `C:\projects\treeminer_xnm\repos\xgpu`, read in full.
- `git log --oneline` on the xgpu repo shows the scripts were actively iterated Oct–Nov 2023 (e.g., commit `2d21170` removed a redundant `apt update`; commit `f59f4cf` added `sudo` to several lines; `vast-autogpu.sh` was renamed twice from `vast-new.sh` → `vast-test-auto-gpu.sh` → `vast-autogpu.sh`), confirming this is a hand-maintained ops script set, not generated/templated code.
- `shanhaicoder/XENGPUMiner` (the actual upstream every script clones) is **not reachable** as of this analysis (`gh api repos/shanhaicoder/XENGPUMiner/contents/` → 404; `https://github.com/shanhaicoder/XENGPUMiner` → 404). §3's endpoint/logic detail is sourced instead from the live fork `JozefJarosciak/XENGPUMiner` (fetched via `raw.githubusercontent.com` and the GitHub API on 2026-08-09), which shares the same default wallet placeholder string that xgpu's `sed` commands target — strong evidence it's the same lineage — but it is today's `main` branch of a fork, not a pinned snapshot of what existed when xgpu's scripts were written, so minor implementation drift (e.g., exact retry counts, file names) versus the historical original is possible.
