# XenBlocks Remote Server API Reference

This sheet documents the remote HTTP service used by the miner at
`http://xenblocks.io`. It is separate from TreeMiner's local statistics server on port
`42069`, the marketplace FastAPI service, and the optional Hash API.

## Documentation discovery

The live remote server does **not** currently publish Swagger or OpenAPI documentation.
The common discovery paths `/docs`, `/redoc`, `/openapi.json`, `/swagger`, `/swagger-ui`,
and `/api-docs` all returned HTTP 404 when checked on 2026-08-10.

Useful inspection commands:

```bash
# Current network difficulty
curl -fsS http://xenblocks.io/difficulty | jq .

# Response headers and status
curl -sS -D - -o /dev/null http://xenblocks.io/difficulty

# Pretty-print JSON without jq
curl -fsS http://xenblocks.io/difficulty | python3 -m json.tool
```

The route inventory below is verified against the reference Flask implementation in
`repos/xenminer/gpage.py`. A checked-in reference is not a versioned contract: the live
deployment can differ. Live observations are labeled explicitly.

## Miner-critical endpoints

### `GET /difficulty`

Returns the current minimum Argon2 memory cost. The value is encoded as a JSON string.

```json
{"difficulty":"100"}
```

| Status | Meaning |
|---:|---|
| 200 | Current difficulty returned |
| 404 | Difficulty record unavailable |
| 5xx/timeout/connection error | Server unavailable; retain the last known value |

The alias `GET /difficulty/{account}` currently returns the same shape. The reference
implementation does not use `account` when selecting difficulty.

### `POST /verify`

Submits one immutable find with `Content-Type: application/json`.

```json
{
  "hash_to_verify": "$argon2id$v=19$m=1100,t=1,p=1$...$...XEN11...",
  "key": "<64 hexadecimal characters>",
  "account": "0x<40 hexadecimal characters>",
  "attempts": "123456",
  "hashes_per_second": "42000.00",
  "worker": "worker-name"
}
```

The server validates the key and salt, checks the PHC `m` against current difficulty,
checks for `XEN11` or `XUNI[0-9]`, applies the XUNI time window, runs
`argon2.verify(key, hash_to_verify)`, and then inserts the record.

| Status | Response meaning | TreeMiner action |
|---:|---|---|
| 200 | Hash verified and reportedly saved | Confirm with `/get_block`; do not trust 200 alone |
| 400 | Duplicate key (`Block already exists, continue`) | Confirm with `/get_block` |
| 400 | Malformed or missing input | Quarantine and inspect |
| 401 | Submitted `m` is below current difficulty | Park until current difficulty is `<=` the find's `m` |
| 401 | XUNI outside its time window | Park for a later window |
| 401 | Argon2 verification failed | Permanently invalid; do not retry |
| 429 | Rate limited | Honor `Retry-After` and retry |
| 5xx/timeout/connection error | Server or transport failure | Retain and retry with backoff |

The difficulty check is `submitted_m < current_difficulty`; equality is not required. A
find with `m=1100` passes that check when the server is at either 100 or 1100.

The 401 difficulty-rejection body embeds the server's **current** difficulty in its
message text as `m={N}` (`gpage.py:416`). TreeMiner parses this as a difficulty
observation, updating its cache without waiting for the next `/difficulty` poll.

The time-window rejection strings are reachable only from XUNI code paths
(`gpage.py:433-435,497`); a XEN11 submission can never receive them (docs/05 §2). If one
ever does, the server semantics have changed — TreeMiner quarantines that record loudly
rather than parking it.

Do not hand-edit `m=` to test another difficulty. The Argon2 hash must be recomputed.

### `GET /get_block?key={key}`

Confirms that a submitted record is actually present. `key` must be 64 hexadecimal
characters.

```bash
curl -fsS --get http://xenblocks.io/get_block \
  --data-urlencode 'key=<64-hex-key>' | jq .
```

```json
{
  "block_id": 123,
  "hash_to_verify": "$argon2id$...",
  "key": "<64-hex-key>",
  "account": "0x<address>",
  "created_at": "<server timestamp>"
}
```

| Status | Meaning |
|---:|---|
| 200 | Record exists in the normal block or XUNI table |
| 400 | Missing or malformed key |
| 404 | Record is absent |

This lookup is required because the reference `/verify` handler can return 200 after its
database insertion retries have failed.

## Public read endpoints

These routes exist in the reference implementation but are not required for mining.

| Method and path | Response/use | Live status on 2026-08-10 |
|---|---|---|
| `GET /get_balance/{account}` | `{"account": ..., "balance": number}` | Responding |
| `GET /get_super_blocks/{account}` | Superblock count for an account | Responding |
| `GET /total_blocks` | `{"total_blocks_top100": number}` | Responding |
| `GET /total_blocks2?account=...` | Normal-block count for an account | Timed out during probe |
| `GET /latest_blockrate` | Latest `{id,date,rate}` record | Responding |
| `GET /get_xuni_counts` | Counts grouped by account | Timed out during probe |
| `GET /blockrate_per_day` | Up to 1,000 account block counts | HTTP 500: backing table absent |
| `GET /top_daily_block_miners` | Up to 500 account block counts | HTTP 500 during probe |
| `GET /hash_rate` | Human-readable HTML network hash-rate page | Responding; not JSON |
| `GET /leaderboard` | Redirect to the XenBlocks explorer | HTTP 302 redirect |

```bash
ACCOUNT='0x0000000000000000000000000000000000000000'

curl -fsS "http://xenblocks.io/get_balance/$ACCOUNT" | jq .
curl -fsS "http://xenblocks.io/get_super_blocks/$ACCOUNT" | jq .
curl -fsS "http://xenblocks.io/total_blocks2?account=$ACCOUNT" | jq .
curl -fsS http://xenblocks.io/latest_blockrate | jq .
```

## Internal/non-miner endpoint

### `POST /validate`

The reference server accepts consensus records containing `total_count`,
`my_ethereum_address`, `last_block_id`, and `last_block_hash`. This endpoint mutates server
state and is not part of the miner submission flow. Do not call it for health checks or
API discovery.

## Operational cautions

- The live service currently uses plain HTTP for miner RPC. Never send a private key.
- Treat `/difficulty` as an observation, not a reservation. `/verify` uses the difficulty
  current when it processes the submission.
- Preserve the complete discovery-time PHC string and embedded `m` unchanged.
- A successful `/difficulty` request does not prove `/verify` or its database is healthy.
- A `/verify` HTTP 200 is provisional until `/get_block` returns 200 for the same key.
- Apply timeouts, durable queuing, bounded backoff, and conservative drain pacing.

