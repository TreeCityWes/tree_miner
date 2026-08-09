# First end-to-end chaos validation — 2026-08-09

Environment: WSL2 Ubuntu 24.04, CUDA 12.6, GCC 13.3, RTX 4070 (sm_89), mock server
(`tests/mockserver/mock_server.py`, gpage.py-faithful) at difficulty 64.

## Run 1 — happy path (100 s)

Real GPU mining at ~600 kH/s. 5 blocks found → journaled (WAL, durable) → submitted →
`/get_block` confirmed → **5/5 Acked**. Server saw exactly 5 verify requests (no retry spam).

## Run 2 — 60 s hard outage mid-mining (180 s total)

Timeline: 30 s normal → server `down` (connections dropped, no HTTP response) for 60 s
while mining continued → server restored → drain.

Result: **13 finds total, 13/13 Acked, zero lost.** One block stored pre-outage; the other
12 accumulated in the journal during/after the outage and drained on recovery. Two records
show `attempt_count > 1` (tried during the outage, backed off, succeeded on retry) — the
circuit breaker kept the rest at a single attempt each instead of hammering a dead server.
No journal write failures, no quarantined records.

This is the exact scenario where upstream XenblocksMiner permanently loses finds
(in-RAM queue, 5 retries → drop) and the reference miner crashes.

Caveats: mock server (no real Argon2 verify server-side), tiny difficulty, single GPU,
short soak. Next gates per PLAN §test-plan: XUNI window chaos, difficulty-spike parking,
lying-200 (insert-fail) reconciliation, long soak, then live canary with a real wallet.
