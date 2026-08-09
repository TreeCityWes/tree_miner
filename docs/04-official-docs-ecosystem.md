# XenBlocks / X1 Ecosystem Research (as of 2026-08-09)

This document summarizes what is publicly documented about the XenBlocks proof-of-work
protocol and the X1 network it feeds into, gathered from the official GitBook, the
official site/explorer, community miners, and public GitHub repos. It exists to inform
the design of `treeminer_xnm`, whose differentiating feature is **storing found
hashes + timestamps locally and resubmitting them when the central submission server
comes back online**.

Every claim below is cited. Where sources disagreed, contradicted each other, or a fetch
returned unreliable/unrelated content, it is flagged explicitly in **Unconfirmed /
Conflicting** call-outs rather than asserted as fact. Do not treat anything in those
call-outs as verified.

---

## 1. Protocol Overview

### 1.1 What XenBlocks is

XenBlocks is described as "the Proof of Work (PoW) component of the X1 network,
featuring a decentralized mining process like Bitcoin," rewarding miners with XNM
tokens for finding valid blocks.
Source: [docs.xenblocks.io](https://docs.xenblocks.io/)

Mining began in September 2023, starting from zero supply.
Source: [docs.xenblocks.io/mining/mining-statistics](https://docs.xenblocks.io/mining/mining-statistics)

### 1.2 Algorithm

- Hashing algorithm: **Argon2id** — the hybrid Argon2 variant that uses the Argon2i
  approach for the first half-pass over memory and Argon2d for subsequent passes,
  per RFC 9106's recommendation when threat profile/side-channel risk is unclear.
  Source: [docs.xenblocks.io/technicals/argon2](https://docs.xenblocks.io/technicals/argon2)
- Argon2 is "memory-hard" — bounded by VRAM speed rather than raw compute — which is the
  stated rationale for ASIC resistance and GPU/CPU accessibility.
  Source: [docs.xenblocks.io/technicals/argon2/argon2-relating-to-memory-and-difficulty](https://docs.xenblocks.io/technicals/argon2/argon2-relating-to-memory-and-difficulty)
- A find occurs when a miner's device produces a 65-character hash sequence containing
  the literal substring **"XEN11"**. Each valid find requires both the **hash** and a
  corresponding **key** proving the work — the two must match, which the server verifies
  before crediting a find ("you can't fake it together with a corresponding key").
  Source: [docs.xenblocks.io/technicals/hashing](https://docs.xenblocks.io/technicals/hashing)
- **Self-custodial hashing**: each mined block is cryptographically signed by
  compacting the miner's Ethereum address into 27 bytes and using it as an Argon2 salt.
  This binds every block permanently to the address that found it, which the docs say
  makes block theft/interception impossible (contrasted with SHA-256, which has no salt
  input). Ownership can only move if the original miner shares their private key.
  Source: [docs.xenblocks.io/technicals/hashing/self-custodial-hashing](https://docs.xenblocks.io/technicals/hashing/self-custodial-hashing)

### 1.3 Block/find types (including superblocks)

Three categories of "find" exist, produced from the same underlying Argon2 search
("merged mining" — the algorithm looks for all three simultaneously without extra cost):

| Type | Requirement | Notes |
|---|---|---|
| Standard XenBlock | 65-char hash containing "XEN11" | Base unit; yields XNM reward |
| **Superblock (X.BLK)** | Hash string with 65+ uppercase letters within 136 total characters | ~1000x rarer than a standard XenBlock; unlimited supply |
| **XUNI** | Same search, but only counts if found in a specific window | Only awarded if discovered in the **:55–:05 window at the top of each hour** (a 10-minute window straddling the hour) |

Sources: [docs.xenblocks.io/technicals/hashing/merged-mining](https://docs.xenblocks.io/technicals/hashing/merged-mining),
corroborated by [xnm.pub](https://xnm.pub/) (independent miner vendor, describing the
same three categories and the same XX:55–XX:05 window).

**This XUNI time-window rule is the closest thing to an official "submission timing"
concept found anywhere in the research** — but note it governs when a hash must be
*found* (i.e., which timestamp the winning hash naturally has), not how late it may be
*submitted* to the server after being found. See Section 5.

### 1.4 Difficulty

- Target block rate: **~60 blocks/minute (~84,600 blocks/day)** network-wide.
  Source: [docs.xenblocks.io/technicals/difficulty/difficulty-adjustment-mechanism](https://docs.xenblocks.io/technicals/difficulty/difficulty-adjustment-mechanism)
- Adjustment rule: if production reaches 72 blocks/min, difficulty is raised by 100; if
  it falls to 48 blocks/min, difficulty is lowered (made easier) by 100. Adjustments are
  described as event-driven (triggered by deviation) rather than on a fixed schedule; no
  exact interval is published.
  Source: same as above.
- Consensus: X1 validators watch block-production timestamps and each propose a
  difficulty delta; the network adopts the **median** of proposals "to ensure fairness
  and mitigate outliers." A verification phase occurs before a new difficulty value is
  applied network-wide, to prevent attacks/errors.
  Source: [docs.xenblocks.io/technicals/difficulty/difficulty-consensus](https://docs.xenblocks.io/technicals/difficulty/difficulty-consensus)
- **Fixed total hashrate model**: unlike Bitcoin/Ethereum, XenBlocks intentionally holds
  total network hashpower constant at roughly **10,000,000 h/s** regardless of miner
  count. As more miners join, each individual miner's allotted hashrate is scaled down
  so the global total stays fixed — described as a deliberate tradeoff for sustainable
  energy use rather than a race for more total compute.
  Source: [docs.xenblocks.io/technicals/fixed-total-hashrate](https://docs.xenblocks.io/technicals/fixed-total-hashrate)
- No official difficulty-fetching API URL was found in the documentation (see Section 2).
  A third-party difficulty tracker gist exists but was not verified in depth:
  [gist.github.com/xenartist/a43d439c635c0278515a15c5e49b946b](https://gist.github.com/xenartist/a43d439c635c0278515a15c5e49b946b)
  (**Unconfirmed** — not independently validated as authoritative or current).

### 1.5 Rewards / Tokenomics (XNM)

- XNM is "the reward token for mining XenBlocks." Miners receive XNM each time they find
  a block.
  Source: [docs.xenblocks.io/mining/xnm](https://docs.xenblocks.io/mining/xnm)
- Reward schedule: **10 XNM/block in year 1**, halving annually (5 in year 2, etc.)
  through year 15.
- Total estimated supply after 15 years: **~620,000,000 XNM**, front-loaded — 310M
  (half the total) is minted in year 1 alone, tapering to ~18,900 XNM by year 15.
  Source: [docs.xenblocks.io/mining/xnm](https://docs.xenblocks.io/mining/xnm); also
  referenced by a third-party supply chart at
  [xen.pub/xnm-release-charts.php](https://xen.pub/xnm-release-charts.php) (not
  independently verified).
- XNM earned via mining can reportedly be **staked on the X1 chain for XN-denominated
  rewards** — i.e., XNM is the PoW-mined asset, XN is described elsewhere as X1's native
  staking/gas coin. Source: search-result synthesis citing
  [xencrypto.io coverage](https://www.xencrypto.io/how-to-mine-xenblocks-xnm-xuni-and-superblocks/)
  (**Unconfirmed** — not found stated this way in the official GitBook itself; token
  naming is inconsistent across sources, see Section 3's conflicting-info note).

---

## 2. Official Endpoints and APIs

This is the weakest-documented area — the official GitBook does **not** publish a
formal API reference page (no `/api` or `/reference` section exists in the docs index).
What could be confirmed came from network-config docs and from reading a legacy
open-source miner's code, not from formal API docs.

### 2.1 Confirmed endpoints

| Purpose | Value | Source |
|---|---|---|
| Network name | XenBlocks | [docs.xenblocks.io/mining/add-network](https://docs.xenblocks.io/mining/add-network) |
| RPC URL (web3/MetaMask config) | `https://xenblocks.io:5556/` | same |
| Chain ID | `100101` | same |
| Currency symbol | XNM | same |
| Address migration (EVM→SVM mapping) tool | `https://xenblocks.io/address_migration` | [docs.xenblocks.io/mining/add-network/address-migration](https://docs.xenblocks.io/mining/add-network/address-migration) |
| Block explorer / leaderboard | `https://explorer.xenblocks.io/` (redirect target of `https://xenblocks.io`) | observed via HTTP 302 from xenblocks.io; also named directly in [docs.xenblocks.io/mining/mining-statistics](https://docs.xenblocks.io/mining/mining-statistics) |
| Third-party stats dashboards | `https://xen.pub/index-xenblocks.php` (aggregate), `https://xen.pub/xblocks.php` (per-miner) | [docs.xenblocks.io/mining/mining-statistics](https://docs.xenblocks.io/mining/mining-statistics) |

The explorer (`explorer.xenblocks.io`) renders as a JS single-page app; a static fetch
could not enumerate its underlying data API calls (would require a browser/network
inspector). **Unconfirmed**: exact leaderboard/explorer JSON API routes.

### 2.2 Hash-submission endpoints (from legacy open-source miner code, NOT official docs)

The original reference miner (`jacklevin74/xenminer`, Python, the earliest known
XenBlocks miner by the protocol's original author) hard-codes these submission
endpoints in `miner.py`:

- `POST http://xenblocks.io/verify` — primary submission endpoint
- `POST http://xenblocks.io:4446/send_pow` — secondary PoW submission endpoint

Retry behavior in that code: on submission failure it retries up to `max_retries = 2`
with a `time.sleep(10)` between attempts, and **only** retries if the HTTP response code
is `500`; any other status breaks the retry loop immediately. A `self.timestamp =
time.time()` is set when a block is created, but that timestamp is **not** included in
the JSON payload sent to the server. There is **no persistent queue or on-disk cache**
in this code — if the process exits or the two retries fail, the found block is simply
lost.
Source: [github.com/jacklevin74/xenminer](https://github.com/jacklevin74/xenminer), file
`miner.py` (fetched via raw.githubusercontent.com).

**This is important context for `treeminer_xnm`**: the "reference" miner behavior is
fire-and-forget with a very short, non-persistent retry window. A local
store-and-forward design is a genuine improvement over documented prior art, not a
reinvention of an existing pattern.

### 2.3 Difficulty API

No official difficulty-fetching HTTP endpoint is documented in the GitBook. The
difficulty-adjustment mechanism is described as being "embedded within the mining
software" itself (each miner computes/derives it) rather than being a value miners pull
from a central REST endpoint. Source:
[docs.xenblocks.io/technicals/difficulty/difficulty-adjustment-mechanism](https://docs.xenblocks.io/technicals/difficulty/difficulty-adjustment-mechanism).
**Unconfirmed**: whether any miner in practice polls a difficulty JSON endpoint (some
community miners may derive it from the submission response instead — not verified).

---

## 3. X1 Network Relationship and Current Status (2026)

### 3.1 Relationship to XenBlocks

XenBlocks is positioned as the PoW "front door" / fiat on-ramp mechanism for the X1
chain: X1 itself is a separate Layer-1 (SVM-compatible — i.e., Solana Virtual Machine
architecture, not EVM), and XenBlocks mining is the mechanism by which new participants
earn XNM without KYC or a centralized exchange, then optionally bridge/stake into X1
proper.
Source: [docs.xenblocks.io/usecases/x1-fiat-on-ramp](https://docs.xenblocks.io/usecases/x1-fiat-on-ramp)

Because X1 is SVM-based while XenBlocks mining addresses are EVM-style, miners must
explicitly map/migrate their EVM mining address to an SVM address via
`https://xenblocks.io/address_migration` to be able to use their mined XNM on X1.
Source: [docs.xenblocks.io/mining/add-network/address-migration](https://docs.xenblocks.io/mining/add-network/address-migration)

X1 was conceived by Jack Levin (Fair Crypto Foundation, also behind the earlier XEN
Crypto token). Development history per third-party coverage: Devnet launched Jan 28,
2022, followed by a "Fastnet" iteration, then testnet.
Source: [X (Twitter) — XEN_Crypto](https://x.com/XEN_Crypto/status/1732909621344694711)
(**secondary source, not the official docs — treat historical dates as approximate**).

### 3.2 Current status as of August 2026

- **X1 mainnet is reported live**, having launched **October 6, 2025**, with **over
  1,000 validators** participating at time of research.
  Source: [docs.x1.xyz](https://docs.x1.xyz/)
- On-chain program for XenBlocks-on-X1 exists as an open-source Solana-style program:
  [github.com/FairCrypto/x1-xenblocks](https://github.com/FairCrypto/x1-xenblocks)
  ("XenBlocks registry on X1 blockchain"). Its README describes three entry points:
  `initialize` (admin-only), `submit_block` (**reserved for a "watchtower"** role —
  i.e., not every miner submits directly on-chain; a designated watchtower service
  relays finds), and `vote_for_block` (public voting mechanism, open to miners, with a
  documented `CannotSelfVote` error condition). Data structures include
  `XenBlocksState`, `XenBlockInfo`, and `VoteInfo`; events `NewBlock` and `NewVote` are
  emitted. **The README does not document any timestamp/deadline/expiry logic for
  submissions** — no code showing a submission time-window could be retrieved (attempts
  to fetch `lib.rs` directly returned 404; only the README was accessible).
  Source: [github.com/FairCrypto/x1-xenblocks](https://github.com/FairCrypto/x1-xenblocks)
  README. **Unconfirmed**: actual on-chain timing/expiry rules in `submit_block`, since
  the source file itself could not be retrieved.

### 3.3 Unconfirmed / Conflicting — naming and status

- **Name collision warning**: "X1 Network" is *also* the name of an unrelated Ethereum
  Layer-2 built by OKX on Polygon CDK technology, which launched its own mainnet in
  early 2024. This is a **different project** from the X1 blockchain described in this
  document (Jack Levin / Fair Crypto Foundation / XenBlocks). Search results conflate
  the two under the same name. Anyone researching this space should double-check which
  "X1" a given source is discussing.
  Sources: search synthesis referencing both
  [Bitget's OKX X1 airdrop page](https://www.bitget.com/airdrop/x1-network-%E2%80%93-testnet)
  and X1/XenBlocks-specific sources above.
- **Native token naming is inconsistent across sources.** The XenBlocks GitBook's own
  network-config page lists the currency symbol as **XNM** for the "XenBlocks" EVM
  network entry. Separately, third-party sources describe **XN** as X1's native
  staking/gas coin ("XN Coin is the native coin of the X1 Blockchain... not listed on
  any exchange as of that writing"), while a separate community wiki
  ([xenartist/x1-wiki](https://github.com/xenartist/x1-wiki)) refers to **XNT** as the
  token staked by X1 validators ("Stake your XNT tokens to secure the X1 blockchain").
  Whether XN and XNT refer to the same asset (rename?) or are genuinely different
  tokens, and how either relates to mined XNM, **could not be confirmed** from official
  docs. Treat any statement equating XNM = XN = XNT as unverified.
- A separate X1-branded project, **"X1 EcoChain"** (`x1ecochain.com`), surfaced in
  search results with its own roadmap citing a **Q3 2026 TGE/mainnet target** — this
  appears to be yet another distinct project sharing the "X1" name, not the Jack
  Levin/XenBlocks X1 chain (which per docs.x1.xyz already launched mainnet in October
  2025). **Flagged as likely a third, unrelated "X1"** rather than reconciled with the
  other two above; not independently confirmed either way.

---

## 4. Ecosystem Tools / Miners

| Tool | Type | Notes | Source |
|---|---|---|---|
| `jacklevin74/xenminer` | Original/reference miner (Python) | Fire-and-forget submission, 2-retry/10s-sleep logic, no persistent local queue (see §2.2) | [GitHub](https://github.com/jacklevin74/xenminer) |
| WoodyMiner (`woodysoil/XenblocksMiner`) | Popular community GPU miner (C++ core, replacing an earlier Python version) | Marketed as "the next evolution," eliminating compile errors of the legacy Python miner; site promises upcoming real-time telemetry dashboard | [woodyminer.com](https://www.woodyminer.com/), [GitHub](https://github.com/woodysoil/XenblocksMiner) |
| XenBlocks Miner by Xen.pub (`xnm.pub`) | CUDA-optimized GPU miner, Windows/Linux (AppImage), HiveOS & Vast.ai guides | Explicitly claims **SQLite-backed persistent block storage with automatic retry logic to prevent data loss from connectivity disruptions** — found blocks are cached locally and resubmitted when connectivity returns. Also supports XUNI-specific modes: "time-restricted" (submit only in the :55–:05 window) vs. "continuous queuing" (queue locally, submit when window opens). Pool connectivity via `--rpcLink` flag. | [xnm.pub](https://xnm.pub/) — **this is the single closest prior-art match to `treeminer_xnm`'s core feature found in this research; verify its actual behavior firsthand before assuming the marketing copy is fully accurate** |
| `tr4vLer/xenvast` | "XenBlocks Mining Toolbox" for Vast.ai rentals | Includes a `--testDifficulty` cap flag; when live network difficulty exceeds the configured cap, found blocks are queued locally in a `blocks.db` SQLite file and only submitted once difficulty drops back under the cap. **Explicitly documented risk: if the instance is shut down before queued blocks are submitted, they can be permanently lost unless `blocks.db` is copied off the machine first** — i.e., local caching exists but isn't crash-safe/durable by design. | [GitHub](https://github.com/tr4vLer/xenvast) (behavior described via search-indexed docs; not independently re-verified against current source) |
| TreeCityWes/XenBlocks-Assistant | Vast.ai deployment automation | Automates renting/configuring/deploying miners on Vast.ai; not itself a submission client | [GitHub](https://github.com/TreeCityWes/XenBlocks-Assistant) |
| TreeCityWes/XenBlocksExplorer | Community-built Python explorer | Alternative to the official explorer | [GitHub](https://github.com/TreeCityWes/XenBlocksExplorer) |
| `beshenkaD/XenBloxMiner`, `beshenkaD/XenDroid` | CPU+GPU miner / Android miner | Surfaced in search only; not investigated in depth | GitHub |
| `shanhaicoder/XENGPUMiner` | GPU miner | Surfaced in search only; not investigated in depth | GitHub |

**Note on reliability of the WoodyMiner README fetch**: repeated attempts to fetch
`woodysoil/XenblocksMiner`'s actual README (both via the GitHub web UI and raw content
URLs on `main` and `master` branches) returned content describing an unrelated-sounding
"hashpower marketplace / fleet monitoring platform" with FastAPI/React/MQTT — which
does not match the C++ GPU-miner description found on woodyminer.com. This is most
likely a tooling/fetch artifact (e.g., a cached or mismatched page) rather than the
repo's real content, but it could not be reconciled within this research session.
**Treat the woodyminer.com summary in this document as the more reliable source for
that project**, and independently verify against the live repo before relying on any
claim about WoodyMiner's submission/retry internals.

---

## 5. Server Reliability, Submission Timing, and Resubmission Tolerance

This is the section most directly relevant to `treeminer_xnm`'s core differentiator.
**No official documentation of an outage history, an SLA, or a formal "how late can a
submission be" rule was found anywhere** — not in the GitBook, not on xenblocks.io, not
in the FairCrypto on-chain program README, and not via targeted web search. Below is
everything adjacent that *was* found, plus an explicit list of what remains unknown.

### 5.1 What is documented

- **XUNI's :55–:05 hourly window** is the one place the official docs define a hard
  timing rule — but it constrains when a hash must be *discovered* to count as a XUNI
  find, not when it must be *submitted* to the server relative to discovery time. A
  hash found at, say, 14:57 could in principle still be submitted seconds or minutes
  later without the docs stating an explicit submission deadline.
  Source: [docs.xenblocks.io/technicals/hashing/merged-mining](https://docs.xenblocks.io/technicals/hashing/merged-mining)
- **No timestamp is included in the legacy reference miner's submission payload**
  (`jacklevin74/xenminer`) despite a timestamp being generated client-side when the
  block is found — meaning that miner, at least, cannot signal "when found" to the
  server at all; the server would only see wall-clock arrival time of the HTTP request.
  Source: `miner.py`, [github.com/jacklevin74/xenminer](https://github.com/jacklevin74/xenminer)
  (see §2.2).
- **`submit_block` on the X1 on-chain program is restricted to a "watchtower" role**,
  implying submissions are relayed/batched through an intermediary rather than every
  miner writing directly to chain — but no timing/expiry logic for that relay could be
  retrieved (the source file 404'd; only the high-level README was accessible).
  Source: [github.com/FairCrypto/x1-xenblocks](https://github.com/FairCrypto/x1-xenblocks)
  README (see §3.2). **Unconfirmed** beyond this.
- **Community precedent for local caching + delayed resubmission already exists**,
  suggesting the ecosystem tolerates at least some delay between find and submission:
  - `xnm.pub`'s miner advertises SQLite-backed persistent storage plus automatic retry
    "when network conditions permit," explicitly to avoid losing finds to connectivity
    issues (§4).
  - `tr4vLer/xenvast` queues blocks in a local `blocks.db` and submits them once a
    difficulty condition is met, which by construction can be minutes to hours after
    the original find — implying the server does accept at least moderately delayed
    submissions in practice, though no explicit outer bound is documented anywhere (§4).
  Neither project documents an outer bound on how old a "found" hash can be and still
  be accepted; both simply state that delayed resubmission works with essentially no
  stated ceiling other than "eventually, when conditions are met."

### 5.2 What could NOT be confirmed (explicit gaps)

- **No published maximum submission delay / expiry window** for a found hash-plus-key
  pair. It is not documented whether the server rejects a valid, correctly-signed find
  submitted an hour, a day, or a week after it was actually found.
- **No documentation of server-side duplicate/replay detection semantics** — e.g.,
  whether resubmitting the same hash twice after a retry is idempotent (safely ignored)
  or would be treated as an error/duplicate-block condition. `treeminer_xnm`'s
  resubmission-after-recovery design should assume this is unverified and build in its
  own duplicate-detection/dedup logic rather than relying on server-side idempotency.
- **No published outage history or uptime/SLA statement** for `xenblocks.io` or its
  submission endpoints (`/verify`, `:4446/send_pow`) was found via search. No status
  page, incident log, or public postmortem surfaced.
- **No official rate-limiting or backoff guidance** beyond what's inferable from the
  legacy miner's own hardcoded 2-retry/10-second behavior (which is the miner's own
  choice, not a documented server requirement).
- **Whether difficulty-at-time-of-find vs. difficulty-at-time-of-submission matters**
  is unknown — i.e., if network difficulty rises between when a hash was found (against
  a since-lowered target) and when it's finally resubmitted after an outage, it is
  undocumented whether the server validates against the difficulty at find-time or at
  submit-time. This is directly relevant to `treeminer_xnm`'s local-storage design and
  could not be resolved from any source in this research pass.

### 5.3 Implication for `treeminer_xnm`

Given the above, `treeminer_xnm`'s "store hash + timestamp locally, resubmit on
recovery" approach:
- Is **not** contradicted by any documented rule (no source states a hard submission
  deadline that would make delayed resubmission invalid).
- Is **not without precedent** — at least two community miners (`xnm.pub`,
  `tr4vLer/xenvast`) already do local caching + delayed resubmission, though neither
  publishes the server-side acceptance guarantees this relies on.
- Should be built defensively, since duplicate-handling, delay-tolerance limits, and
  difficulty-at-validation-time are all unconfirmed/undocumented server behaviors — i.e.
  treat the server's actual tolerance as an empirical unknown to be discovered
  (carefully, respecting rate limits) rather than a documented contract.

---

## 6. Source List

- https://docs.xenblocks.io/ (and its `.md`/`llms.txt`/`llms-full.txt` variants)
- https://docs.xenblocks.io/mining/mining-xnm-with-xenblocks
- https://docs.xenblocks.io/mining/xnm
- https://docs.xenblocks.io/mining/add-network
- https://docs.xenblocks.io/mining/add-network/address-migration
- https://docs.xenblocks.io/mining/mining-statistics
- https://docs.xenblocks.io/technicals/hashing
- https://docs.xenblocks.io/technicals/hashing/merged-mining
- https://docs.xenblocks.io/technicals/hashing/self-custodial-hashing
- https://docs.xenblocks.io/technicals/fixed-total-hashrate
- https://docs.xenblocks.io/technicals/argon2
- https://docs.xenblocks.io/technicals/argon2/argon2-relating-to-memory-and-difficulty
- https://docs.xenblocks.io/technicals/difficulty
- https://docs.xenblocks.io/technicals/difficulty/difficulty-adjustment-mechanism
- https://docs.xenblocks.io/technicals/difficulty/difficulty-consensus
- https://docs.xenblocks.io/usecases/x1-fiat-on-ramp
- https://xenblocks.io (redirects to https://explorer.xenblocks.io/)
- https://xenblocks.io/address_migration
- https://www.woodyminer.com/
- https://github.com/woodysoil/XenblocksMiner
- https://xnm.pub/ and https://xnm.pub/tutorials.html
- https://github.com/jacklevin74/xenminer (incl. `miner.py`)
- https://github.com/FairCrypto/x1-xenblocks
- https://github.com/tr4vLer/xenvast
- https://github.com/TreeCityWes/XenBlocks-Assistant
- https://github.com/TreeCityWes/XenBlocksExplorer
- https://github.com/JozefJarosciak/X1 (incl. `Mine-XenBlocks.md`)
- https://docs.x1.xyz/
- https://github.com/xenartist/x1-wiki
- https://gist.github.com/xenartist/a43d439c635c0278515a15c5e49b946b
- https://xen.pub/index-xenblocks.php, https://xen.pub/xblocks.php, https://xen.pub/xnm-release-charts.php
- https://www.xencrypto.io/how-to-mine-xenblocks-xnm-xuni-and-superblocks/
- https://x.com/XEN_Crypto/status/1732909621344694711 (secondary/historical color)
- https://www.bitget.com/airdrop/x1-network-%E2%80%93-testnet (unrelated OKX "X1 Network" — name-collision warning, see §3.3)
- https://x1ecochain.com/ (possibly a third, unrelated "X1" project — see §3.3)

---

## 7. Summary of Unconfirmed Items (quick reference)

1. Exact leaderboard/explorer JSON API routes (SPA, not statically inspectable).
2. Whether any current miner polls a difficulty REST endpoint vs. deriving it locally.
3. Timing/expiry logic inside `x1-xenblocks`'s on-chain `submit_block` (source file
   inaccessible; only README summary available).
4. Relationship/identity between XNM, XN, and XNT token symbols across sources.
5. Whether "X1 EcoChain" (Q3 2026 TGE) is the same project as the Jack
   Levin/FairCrypto X1 chain (mainnet live since Oct 2025) or a distinct project.
6. Any maximum submission delay / expiry window for a found hash+key pair.
7. Server-side duplicate/replay handling for resubmitted finds.
8. Any public outage history, status page, or SLA for xenblocks.io submission
   endpoints.
9. Whether validation uses difficulty-at-find-time or difficulty-at-submit-time for
   delayed submissions.
10. WoodyMiner's actual current README/source content (fetch attempts returned
    content inconsistent with the project's own marketing site — see §4 note).
