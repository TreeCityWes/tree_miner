# TreeMiner

TreeMiner is a XenBlocks miner: a fork of [woodysoil/XenblocksMiner](https://github.com/woodysoil/XenblocksMiner)
(MIT — the CUDA Argon2id engine, difficulty poller, and build system are Woody's work, and the
credit for the hashing core belongs to him).

**What makes it different: no found hash is lost to a server outage.** Upstream keeps pending
submissions in an in-RAM queue and drops them after a handful of failed retries; the XenBlocks
endpoint goes down often enough that this is the single largest source of silent loss. TreeMiner
replaces that layer wholesale:

- **Durable find journal** — every find is written to SQLite (WAL, `synchronous=FULL`) *before*
  anything else happens to it, and survives crashes, reboots, and multi-hour outages. An
  append-only fsync'd JSONL fallback sink catches even journal-write failures.
- **Response classifier** — server replies are parsed into real states (accepted, duplicate,
  difficulty-parked, XUNI-window-parked, quarantined, permanently invalid) instead of a blind
  retry count.
- **Circuit breaker + adaptive drain** — backs off while the server is down, then drains the
  backlog at a rate that ramps with observed health instead of a fixed trickle.
- **Mandatory `/get_block` confirmation** — the server can answer `200` when its own insert
  failed, so an accepted find is only `Acked` after the block is confirmed by key *and* hash.
- **XUNI window budgeting** — XUNI finds outside the :55–:05 window are parked and replayed in a
  later window rather than thrown away, and are fetched per-kind so they cannot starve XEN11.
- **Persistent difficulty cache** — last-known difficulty is restored at startup, so an outage no
  longer means mining at the hardcoded 42069 fallback (~50x the real cost) until the server
  answers.

On top of that it ships a **CPU mining sidecar**, a **full-screen terminal console**, and a
**LAN web console** for rig operators.

See `PLAN.md` for the design authority and `CHANGES-FROM-UPSTREAM.md` for the divergence log.

## Build

C++17 + CUDA, built with CMake + vcpkg. Full instructions (Linux and Windows, prerequisites,
troubleshooting): [doc/BUILD_INSTRUCTIONS.md](./doc/BUILD_INSTRUCTIONS.md).

```bash
sudo apt install build-essential tar curl zip unzip git cmake ninja-build
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics

cd treeminer
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/custom-triplets" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-static
cmake --build build-native --parallel "$(nproc)"
ctest --test-dir build-native --output-on-failure
```

The CUDA architecture of the GPU visible at configure time is auto-detected (the configure log
prints `-- TreeMiner CUDA architectures: <sm>`). CMake presets are available for pinned
architectures and for a fat binary that covers several tiers in one build:

```bash
cmake --preset cuda-release-linux-fat      # multi-arch fat binary
cmake --preset cuda-release-vcpkg-sm86     # single-arch, smallest/fastest to build
cmake --build --preset cuda-release-vcpkg-sm86
```

## Run

```bash
./xenblocksMiner \
  --minerAddr 0xYourEthereumAddress \
  --cudaStreams 2 \
  --cpuWorkers 4 \
  --display terminal \
  --dashboard-port 42069
```

| Flag | Meaning |
|---|---|
| `--minerAddr` | Ethereum address credited with the blocks (also settable in `config.txt`). |
| `--cudaStreams` | Independent CUDA work streams per device (1–2). |
| `--cpuWorkers` | CPU sidecar mining workers; `0` disables. CPU workers only hash while difficulty is at or below the ceiling (100 by default) and auto-resume when it falls back. |
| `--display` | `logs` (default), `terminal`, or `prompt` — see below. |
| `--dashboard-bind` | Web console listen IP. Default `0.0.0.0`; use `127.0.0.1` to keep it private. |
| `--dashboard-port` | Web console port (default `42069`). |
| `--journalPath` | Where the find journal lives (default `treeminer-journal.db`, relative to the working directory). The resolved absolute path is logged at startup. |

Settings also read from `config.txt` (`dashboard_bind`, `dashboard_port`, `journal_path`, …);
a command-line value overrides the file.

### Terminal display modes

- `--display logs` — the scrolling event stream. **Default**, so unattended restarts never block
  waiting on stdin.
- `--display terminal` — a full-screen operator console on the terminal alternate screen,
  redrawn at 2 fps, with a bounded recent-event rail. Ctrl-C restores the original shell and
  scrollback.
- `--display prompt` — ask at boot which of the two to use.

### Local web console

The miner serves a read-only console at `http://<rig-ip>:42069/`: live hashrate sparkline with
find markers, journal and delivery state, per-device telemetry, four switchable themes persisted
in `localStorage`, and a machine-readable `/api/rig` JSON endpoint. The page is self-contained
(no CDN, no external fonts) so it still renders when the rig's upstream network is down — which
is exactly when an operator looks at it.

**It binds `0.0.0.0` by default and has no authentication.** That default is deliberate: rigs on a
LAN, on Vast.ai, or inside Docker are otherwise unreachable without extra plumbing. It exposes
wallet address, hardware, performance, and operational details to anyone who can reach the port,
so on an untrusted network either make it private:

```bash
xenblocksMiner --dashboard-bind 127.0.0.1     # or dashboard_bind=127.0.0.1 in config.txt
```

and reach it over an SSH tunnel (`ssh -L 42069:127.0.0.1:42069 user@rig`), or firewall the port at
the host/network. IP literals are validated at startup, IPv6 browser URLs are bracketed, and the
startup banner prints both the usable URL and the actual listen address.

Other routes served by the miner: `/healthz`, `/stats` (HiveOS-compatible), `/api/v1/status`,
`/platform/status`, and `/assets/hashfield.webp`.

## Precompiled binaries

Precompiled miner binaries for supported platforms are available in the upstream
[Releases](https://github.com/woodysoil/XenblocksMiner/releases) section.

## Remote mining protocol

[XenBlocks remote server API reference](docs/xenblocks-remote-api.md).

---

# Upstream platform (inherited from Woody's repo)

Everything below describes the marketplace/monitoring platform that ships in the upstream
repository. It is **not** part of the Phase 1 TreeMiner deliverable (PLAN §9 strips MQTT,
marketplace, and telemetry paths from the default binary) and is retained here for reference and
for anyone who wants to run it.

[![Python](https://img.shields.io/badge/Python-3.10+-3776ab?logo=python&logoColor=white)](https://python.org)
[![React](https://img.shields.io/badge/React-18-61dafb?logo=react&logoColor=white)](https://react.dev)
[![TypeScript](https://img.shields.io/badge/TypeScript-5.7-3178c6?logo=typescript&logoColor=white)](https://typescriptlang.org)
[![TailwindCSS](https://img.shields.io/badge/Tailwind_CSS-4-06b6d4?logo=tailwindcss&logoColor=white)](https://tailwindcss.com)

## Overview

A hashpower marketplace and mining monitoring platform for the XEN ecosystem: a real-time mining
fleet dashboard plus a marketplace where providers list GPU capacity and renters lease hashpower
on demand. It uses an embedded MQTT broker for worker telemetry, WebSocket-driven live updates,
and JWT-authenticated wallet-based accounts.

```
Browser
  |
  v
Vite Dev Server (:5173)
  |
  v
React SPA (TanStack Query + React Router)
  |
  +--> REST API (:8080/api/...)  ---+
  |                                 |
  +--> WebSocket (:8080/ws)  -------+--> Python Backend (FastAPI + uvicorn)
                                           |
                                    +------+------+
                                    |             |
                                 SQLite      MQTT Broker (:1883)
                              (aiosqlite)        |
                                           Mining Workers
```

| Backend | Frontend |
|---|---|
| Python 3.10+ | React 18 |
| FastAPI + uvicorn | TypeScript 5.7 |
| SQLite (aiosqlite) | Vite 6 |
| Embedded MQTT broker | TailwindCSS v4 |
| JWT auth (PyJWT) | TanStack Query v5 |
| Pydantic v2 | Lightweight Charts, Recharts |
| eth-account | Sonner, Radix UI, React Router v7 |

## Quick start

Prerequisites: Python 3.10+, Node.js 18+, npm or yarn.

```bash
# Backend — embedded MQTT broker on :1883, REST API on :8080
pip install -r server/requirements.txt
./scripts/run_mock_server.sh
# or: python -m server.server --mqtt-port 1883 --api-port 8080

# Frontend — dashboard at http://localhost:5173, proxies API calls to the backend
cd web && npm install && npm run dev

# Test data
python scripts/mock_fleet.py --workers 10 --broker localhost --port 1883
python scripts/generate_test_data.py
```

The fleet simulator spawns N virtual workers that connect over MQTT and emit realistic telemetry
(registration, 30 s heartbeats, block-found messages, random offline/online transitions; GPU
profiles from RTX 3060 Ti to H100), e.g.
`python scripts/mock_fleet.py --workers 20 --broker localhost --port 1883 --block-interval 60`.
`scripts/generate_test_data.py` populates SQLite with historical workers, blocks, and marketplace
activity. Production frontend build: `cd web && npm run build` (outputs to `web/dist/`).

## Pages

| Page | Route | Description |
|---|---|---|
| **Overview** | `/` | Fleet summary dashboard -- worker counts, total hashrate, block production, and recent activity table. |
| **Monitoring** | `/monitoring` | Real-time fleet monitoring with hashrate charts, per-worker status table, and block history. Uses WebSocket for live updates. |
| **Marketplace** | `/marketplace` | Browse available hashpower listings from providers. Filter by GPU type, price, and availability. |
| **Provider** | `/provider` | Provider management console -- list/delist GPU capacity, monitor active leases, view earnings charts via Lightweight Charts. |
| **Renter** | `/renter` | Renter dashboard for browsing, leasing, and managing active hashpower rentals. Requires wallet connection. |
| **Account** | `/account` | Wallet-based account management -- connect wallet, view balances, and manage authentication. |

## Project structure

```
treeminer/
├── src/                     # C++/CUDA miner core
│   ├── main.cpp             # Entry point, CLI/config, journal-first wiring
│   ├── kernelrunner.cu      # CUDA Argon2id engine (upstream, zero diffs)
│   ├── CpuMiningWorker.*    # CPU mining sidecar
│   ├── TerminalUi.cpp       # Full-screen operator console
│   ├── LocalServer.cpp      # Local HTTP server (console + JSON routes)
│   ├── DashboardPage.h      # Embedded self-contained dashboard page
│   ├── journal/             # Durable SQLite FindJournal + fallback sink
│   ├── submit/              # Classifier, breaker, drain scheduler, submitter
│   ├── treeminer/           # Shared journal/submitter contract types
│   ├── hashapi/             # Reusable Hash API + Argon2 self-test
│   └── platform/            # MQTT/marketplace plumbing (incl. CommandEnvelope)
├── tests/                   # Unit, chaos, and mock-server suites
├── doc/                     # Build instructions, API docs
├── docs/                    # Miner-side design/optimization docs
├── proto/                   # Protocol definitions
├── server/                  # Python backend (upstream platform)
│   ├── server.py            # Entry point (MQTT + API orchestration)
│   ├── broker.py            # Embedded async MQTT broker
│   ├── storage.py           # SQLite persistence layer
│   ├── watcher.py           # Block watcher / telemetry ingestion
│   ├── monitoring.py        # Fleet monitoring service
│   ├── matcher.py           # Hashpower order matching engine
│   ├── settlement.py        # Lease settlement engine
│   ├── pricing.py           # Dynamic pricing engine
│   ├── reputation.py        # Provider reputation scoring
│   ├── account.py           # Wallet-based account management
│   ├── auth.py              # JWT authentication
│   ├── ws.py                # WebSocket connection manager
│   └── routers/             # FastAPI route modules
├── web/                     # React frontend (upstream platform)
│   └── src/                 # pages/, design/, hooks/, utils/, context/, lib/
└── scripts/                 # Development & testing utilities
    ├── mock_fleet.py        # Simulated mining fleet (MQTT workers)
    ├── generate_test_data.py
    ├── run_mock_server.sh
    ├── demo.sh
    └── test_cpp_integration.sh
```

## License

MIT. Derived from woodysoil/XenblocksMiner; upstream's MIT notice is retained.
