# Mock XenBlocks server (reference `gpage.py` semantics)

`mock_server.py` is a **stdlib-only** (Python 3, `http.server`) stand-in for the XenBlocks
verification server, replicating the reference implementation
(`repos/xenminer/gpage.py`, `/verify` handler at lines 366–520) — same response strings,
same status codes, same check order — plus fault-injection controls for chaos tests.

It is deliberately separate from `treeminer/server/` and `scripts/run_mock_server.sh`,
which model Woody's own marketplace server, not the reference protocol.

## Run

```sh
python tests/mockserver/mock_server.py [--port 8545] [--host 127.0.0.1] [--difficulty 100000]
```

## Protocol endpoints

| Endpoint | Behavior (gpage.py line refs) |
|---|---|
| `POST /verify` | Full verify pipeline: key hex check (:390), salt format (:394), missing fields (:398), strictly-`<` difficulty on embedded `m=` (:412), target scan on last 87 chars (:421-442), XUNI window (:429-435), duplicate key (:507-510), insert + success (:515) |
| `GET /get_block?key=<64-hex>` | 400 missing/invalid key, 404 absent, 200 with the stored row (:331-364) |
| `GET /difficulty` (+ `/difficulty/<account>`) | `{"difficulty": "<N>"}` — note: a JSON **string** (:109-117) |

Exact response strings served (the classifier truth table):

| Code | Body |
|---|---|
| 200 | `{"message": "Hash verified successfully and block saved."}` — **also returned in `insert-fail` mode with nothing stored** (the unconfirmed-200, :492-494,515) |
| 400 | `{"message": "Block already exists, continue"}` (duplicate key, :510) |
| 400 | `{"error": "Invalid key format"}` / `{"error": "Invalid salt format"}` / `{"error": "Missing hash_to_verify, key, or account"}` |
| 401 | `{"message": "Hash does not contain 'm=<N>'. Your memory_cost setting in your miner will be autoadjusted."}` (:416) |
| 401 | `{"message": "XUNI Submitted outside of proper time frame."}` (:434) |
| 401 | `{"message": "XUNI found outside of time window"}` (legacy branch, :497 — reachable via `xuni_race`) |
| 401 | `{"message": "Hash does not contain any of the valid targets ['XEN11'] in the last 87 characters. Adjust target_substr in your miner."}` (:439) |
| 401 | `{"message": "Hash verification failed."}` (:519 — armed via `verify-fail`) |

## Control endpoints (test-only; never affected by fault modes)

`GET /control` — current state plus stored-row counts:

```json
{"mode": "normal", "difficulty": 100000, "xuni_window": "auto", "xuni_race": false,
 "timeout_seconds": 30.0, "stored_blocks": 2, "stored_xuni": 0, "verify_requests": 5}
```

`POST /control` with a JSON object; any subset of:

| Field | Values | Effect |
|---|---|---|
| `mode` | `normal` | default behavior |
| | `down` | drop every connection abruptly (no HTTP response — clients see reset/EOF) |
| | `timeout` | stall `timeout_seconds` before answering (defeats client timeouts) |
| | `empty-body` | 200 with a zero-length body |
| | `500` | 500 `{"message": "Internal Server Error"}` on every request |
| | `insert-fail` | `/verify` returns 200 success **without storing** — `/get_block` will 404 (the unconfirmed-200 chaos mode) |
| | `verify-fail` | `/verify` returns 401 `"Hash verification failed."` |
| `difficulty` | integer | set current difficulty (drives the 401 difficulty message) |
| `xuni_window` | `auto` \| `open` \| `closed` | override the `:55–:05` server-clock window (`auto` uses the real local clock, gpage.py:36-40) |
| `xuni_race` | boolean | force the two window checks to disagree (first passes, second fails), producing the legacy 401 `"XUNI found outside of time window"` (:497) |
| `timeout_seconds` | number | stall duration for `mode=timeout` |
| `reset` | boolean | clear tables/counters and return to `normal` (add `"keep_difficulty": true` to preserve difficulty) |

Fault modes `down`, `timeout`, `empty-body`, and `500` apply to **all** protocol endpoints
(`/verify`, `/get_block`, `/difficulty`); `insert-fail` and `verify-fail` only alter `/verify`.
`/control` always works, so a chaos script can recover the server it just "killed".

### Examples

```sh
# lie: accept but do not store (200 followed by /get_block 404)
curl -s -X POST localhost:8545/control -d '{"mode": "insert-fail"}'

# outage for 30 s, then recovery
curl -s -X POST localhost:8545/control -d '{"mode": "down"}'; sleep 30
curl -s -X POST localhost:8545/control -d '{"mode": "normal"}'

# difficulty spike +3000 (parks in-flight finds), later lowered (auto-unpark)
curl -s -X POST localhost:8545/control -d '{"difficulty": 103000}'
curl -s -X POST localhost:8545/control -d '{"difficulty": 100000}'

# force the XUNI window shut regardless of wall clock
curl -s -X POST localhost:8545/control -d '{"xuni_window": "closed"}'
```

## Deliberate deviations from `gpage.py` (all documented in the module docstring)

1. **No argon2**: stdlib-only, so `argon2.verify` is not executed; submissions are treated
   as verified unless `verify-fail` is armed. Structural checks (salt, `m=`, targets) still run.
2. **No EIP-55 checksum** on the decoded salt address (needs keccak); any salt that
   base64-decodes to 40 hex chars passes, as does the legacy `WEVO...` salt.
3. **Oversize hashes**: gpage.py nulls `hash_to_verify` > 150 chars *before* its explicit
   length check, so oversize submissions actually hit the salt-check `TypeError` → 500 path;
   the 401 length message (:444-448) is unreachable. Replicated faithfully (500), with the
   dead length check kept for parity.
4. **Crash parity**: paths where gpage.py would raise `TypeError` into a Flask 500
   (missing `key`, missing `hash_to_verify`, no `m=` in the hash) return
   `{"message": "Internal Server Error"}` with status 500.
