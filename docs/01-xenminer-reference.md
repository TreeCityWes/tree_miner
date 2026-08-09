# XenBlocks Reference Implementation — Technical Analysis

**Source:** `jacklevin74/xenminer`, cloned at `C:\projects\treeminer_xnm\repos\xenminer`
**HEAD:** `779aa37` — *"Add minimum difficulty floor and fix is_within_five_minutes_of_hour missing parentheses"*
**Purpose of this document:** establish exactly how the reference miner mines and submits, what the server validates, and — critically — whether a hash found while the central server is offline can be stored and resubmitted later.

> **Headline answer for our offline-queue feature:** Yes, delayed resubmission is feasible for XEN11 (XNM) blocks. There is **no client-supplied timestamp anywhere in the submission payload**, and **no server-side comparison of submission time against any claimed mine time**. All timestamps are server-set (`DEFAULT CURRENT_TIMESTAMP` at INSERT). The *only* time-coupled rejection paths are (a) the **difficulty floor** — the `m=` embedded in the stored hash must still be `>=` the network difficulty at resubmission time, and (b) **XUNI's hourly window**, which is evaluated against the *server's* clock at receive time and therefore makes XUNI effectively non-queueable. Details in §5.

---

## 1. Mining Algorithm

### 1.1 Hash construction

The miner is a pure Argon2id brute-forcer. There is no block header, no previous-hash binding, no nonce range assignment — each attempt is independent and self-contained.

`repos/xenminer/miner.py:231-255`:

```python
def mine_block(stored_targets, prev_hash, address):
    remove_prefix_address = address[2:]
    salt = bytes.fromhex(remove_prefix_address)

    argon2_hasher = argon2.using(time_cost=difficulty, salt=salt,
                                 memory_cost=memory_cost, parallelism=cores,
                                 hash_len=64)
    ...
        random_data = generate_random_sha256()
        hashed_data = argon2_hasher.hash(random_data)
```

| Element | Value | Source |
|---|---|---|
| Algorithm | **Argon2id**, `v=19` | `passlib.hash.argon2` default type (`miner.py:4`); confirmed by every sample hash string in the repo |
| `time_cost` (`t`) | `config.conf → difficulty`, shipped value **1** | `miner.py:69`, `config.conf:2` |
| `memory_cost` (`m`) | KiB; **dynamic**, fetched from server; shipped bootstrap value **1500** | `miner.py:70`, `config.conf:3`, `miner.py:139-147` |
| `parallelism` (`p`) | `config.conf → cores`, shipped value **1** | `miner.py:71`, `config.conf:4` |
| `hash_len` | **64 bytes** → 86 base64 chars (unpadded) | `miner.py:239` |
| **salt** | **raw 20 bytes of the miner's Ethereum address** (`bytes.fromhex(account[2:])`) → 27 base64 chars unpadded | `miner.py:237` |
| **key / preimage** | `generate_random_sha256()` — a random string of 1–128 chars drawn from `ascii_letters + digits + punctuation`, then SHA-256 hex-digested → **64 lowercase hex chars** | `miner.py:149-155` |

Resulting encoded hash (this is `hash_to_verify`):

```
$argon2id$v=19$m=<M>,t=<T>,p=<P>$<salt_b64_27>$<digest_b64_86>
```

Splitting on `$` yields exactly **6 parts** (leading empty string, `argon2id`, `v=19`, `m=..,t=..,p=..`, salt, digest). The server hard-requires this — `gpage.py:287-289`.

Two salt generations exist and both are accepted:
- **Legacy/global salt:** literal `WEVOMTAwODIwMjJYRU4` (base64 of `XEN10082022XEN`) — a single shared salt, pre-per-account.
- **Current per-account salt:** 27-char base64 that decodes to 20 bytes forming an EIP-55-checksummable address.

Worked examples of both live in `repos/xenminer/utils/salt_unittest.py:66-75`.

### 1.2 What constitutes a valid find

`miner.py:258-274` — the target scan runs against **the last 87 characters** of the encoded hash, i.e. `"$" + the 86-char base64 digest`. Only the digest body counts; the parameter header can never trigger a false positive.

```python
for target in stored_targets:                       # ['XEN11', 'XUNI']  (miner.py:340)
    if target in hashed_data[-87:]:
        if re.search("XUNI[0-9]", hashed_data) and is_within_five_minutes_of_hour():
            found_valid_hash = True
            break
        elif target == "XEN11":
            found_valid_hash = True
            last_element = hashed_data.split("$")[-1]
            hash_uppercase_only = ''.join(filter(str.isupper, last_element))
            if len(hash_uppercase_only) >= 50:
                print(f"{RED}Superblock found{RESET}")
            break
        else:
            found_valid_hash = False
            break
```

| Find type | Rule | Reward token | Time restriction |
|---|---|---|---|
| **XEN11 block (XNM)** | literal substring `XEN11` present in `hash[-87:]` | XNM | **None** |
| **XUNI** | regex `XUNI[0-9]` present in `hash[-87:]` | XUNI | **Only within minutes `[0,5)` or `[55,60)` of the hour** — enforced client-side (`miner.py:93-96`) *and* server-side (`gpage.py:36-40`, `gpage.py:433-435`) |
| **Superblock (X.BLK)** | a XEN11 block whose **digest body** (`hash.split("$")[-1]`) contains **≥ 50 uppercase letters** | X.BLK | None (derived offline) |

**Superblock note.** The superblock rule is *not* evaluated at submission time. `/verify` never counts capitals. Superblocks are recomputed periodically from stored blocks by `make_superblocks.py:39-46`:

```python
last_element = hash_to_verify.split("$")[-1]
hash_uppercase_only = "".join(filter(str.isupper, last_element))
capital_count = len(hash_uppercase_only)
if capital_count >= 50:
    super_block_counts[account_to_update] += 1
```

Because the base64 alphabet is `A-Za-z0-9+/`, only `A-Z` count. The miner's own print at `miner.py:268` uses the identical rule — good. (Historical variant in `miner_test.py:267-268` used "≥65 capitals after stripping digits" — that is **dead/obsolete** and does not match the server.)

**Consequence for us:** a superblock is just a XEN11 block. Storing and resubmitting it late loses nothing; superblock credit is granted whenever `make_superblocks.py` next sweeps the `blocks` table.

### 1.3 Difficulty mechanism

**Difficulty == Argon2 `memory_cost` (KiB).** `t` and `p` are static config. Raising difficulty means raising memory per hash.

**Client side** (`miner.py:128-147`):

```python
def update_memory_cost_periodically():
    time.sleep(10)                       # start checking 10 s after launch
    while True:
        updated_memory_cost = fetch_difficulty_from_server()
        if updated_memory_cost != memory_cost:
            print(f"Updating difficulty to {updated_memory_cost}")
        time.sleep(60)                   # poll every 60 s

def fetch_difficulty_from_server():
    try:
        response = requests.get('http://xenblocks.io/difficulty')
        response_data = response.json()
        return str(response_data['difficulty'])
    except Exception as e:
        print(f"An error occurred while fetching difficulty: {e}")
        return memory_cost               # last value if fetching fails
```

The mining loop checks every 100 attempts and **abandons the current round** when difficulty changes (`miner.py:248-252`):

```python
if attempts % 100 == 0:
    if updated_memory_cost != memory_cost:
        memory_cost = updated_memory_cost
        print(f"{BLUE}Continuing to mine blocks with new difficulty{RESET}")
        return                            # returns None → main loop restarts round
```

> **Bug worth reproducing-or-not in our miner:** `fetch_difficulty_from_server` returns a **`str`** on success (`miner.py:144`) but the **`int`** `memory_cost` on failure (`miner.py:147`). `memory_cost` is initialised as `int` (`miner.py:70`), so the very first comparison `"8" != 1500` is trivially true and `memory_cost` becomes a string thereafter. `updated_memory_cost` is also pre-seeded to the int `1500` (`miner.py:126`). This causes spurious "difficulty changed" restarts. Our miner should normalise to `int` on both paths.

**Server side.** `/difficulty` reads a flat file (`gpage.py:67-76`):

```python
def get_difficulty(account=None):
    file_path = "/home/ubuntu/mining/diff2.chain"
    try:
        new_difficulty_level = int(read_difficulty_level(file_path))
    except (TypeError, ValueError):
        new_difficulty_level = 100000
    return str(new_difficulty_level)
```

The file is written by the difficulty controller `manage_difficulty2.py:16-74`, which runs every **300 s** (`manage_difficulty2.py:92`):

- Reads the latest block rate from `difficulty.db → blockrate` (produced by `indexing/rate_daemon.py:25-44`, which computes blocks/minute over the last 300 blocks' `created_at`, every 60 s).
- **Target rate: 70 blocks/minute**, ±20% band → accept `[56, 84]`; outside the XUNI window the lower limit is tightened to `61` (`manage_difficulty2.py:48-57`).
- Adjustment is **asymmetric**: `+1000` when too fast, `-2000` when too slow (`manage_difficulty2.py:61-65`).
- **Floor `MIN_DIFFICULTY = 100`** (`manage_difficulty2.py:6, 68`) — added in HEAD commit.
- Writes the new level to `/home/ubuntu/mining/diff2.chain` (`manage_difficulty2.py:74-75`).

(The older `indexing/manage_difficulty.py:29-36` targeted 60 blocks/min with symmetric ±100 steps — superseded.)

**Rate of change matters enormously for our queue:** difficulty can rise at most **+1000 KiB per 5-minute tick**. See §5.3.

---

## 2. Submission Protocol

### 2.1 Endpoint inventory (what the reference miner talks to)

| # | Method | URL | Called from | Handler |
|---|---|---|---|---|
| 1 | `GET` | `http://xenblocks.io/difficulty` | `miner.py:142` | `gpage.py:109-117` |
| 2 | `POST` | `http://xenblocks.io/verify` | `miner.py:311` | `gpage.py:366-519` |
| 3 | `GET` | `http://xenminer.mooo.com:4445/getblocks/lastblock` (`config.conf:6`, read at `miner.py:72`, used at `miner.py:161-166`) | `submit_pow` | `getblocks2.py:24-58` |
| 4 | `POST` | `http://xenblocks.io:4446/send_pow` | `miner.py:212` | `indexing/merkleroot.py:12-34` |

Everything else in the repo is server-to-server or explorer-facing and is **not** required by a miner.

---

### 2.2 `GET /difficulty`

Also aliased as `GET /difficulty/<account>` (`gpage.py:110`) — the `account` argument is accepted and ignored (`gpage.py:67`). Difficulty is global, not per-account.

**Response 200:**
```json
{"difficulty": "80000"}
```
Note the value is a **string**, not an int (`gpage.py:76`, `gpage.py:115`).

**Response 404:** `{"error": "Difficulty level not found."}` — unreachable in practice since `get_difficulty` always returns a truthy string.

---

### 2.3 `POST /verify` — the main submission

**Payload the reference miner sends** (`miner.py:289-296`):

```python
payload = {
    "hash_to_verify": hashed_data,        # full "$argon2id$v=19$m=..$salt$digest"
    "key": random_data,                   # 64 hex chars, the argon2 preimage
    "account": account,                   # "0x..." EIP-55 address
    "attempts": attempts,                 # int
    "hashes_per_second": hashes_per_second,  # float
    "worker": worker_id                   # int or None (from --worker)
}
```

**What the server actually reads** (`gpage.py:369-386`):
- `hash_to_verify` — required
- `key` — required
- `account` — required
- `attempts` — read into a local (`gpage.py:386`) but **the only consumer is commented out** (`gpage.py:499-500`), so it is discarded
- `worker_id` — **field-name mismatch**: the miner sends `worker`, the server reads `worker_id` (`gpage.py:370`), and then requires it be a `str` of length ≤ 3 or sets it to `None`. It is never persisted anyway.
- `hashes_per_second` — **not read at all**

> **Design note:** `attempts`, `hashes_per_second`, and `worker` are decorative. Our offline queue can persist and replay whatever values it likes (or omit them) without affecting acceptance. There is **no field in the payload for a timestamp.**

**Validation pipeline, in execution order** (`gpage.py:369-519`):

| Step | Line | Check | Failure |
|---|---|---|---|
| 1 | `370-373` | `worker_id` must be `str`, `len ≤ 3` | silently → `None` |
| 2 | `375-376` | `hash_to_verify` truthy and `len ≤ 150` | → `None` (then step 3 crashes) |
| 3 | `377` | `re.search('XUNI[0-9]', hash_to_verify[-87:])` | ⚠ runs **before** the missing-field guard — a missing/oversized `hash_to_verify` raises `TypeError` here → HTTP 500 |
| 4 | `378-379` | `key` truthy and `len ≤ 128` | → `None` |
| 5 | `380-384` | `account` lowercased, `'` and `"` stripped, `len ≤ 43` | → `None` |
| 6 | `390-391` | `is_hexadecimal(key)` — regex `^[a-fA-F0-9]*$` | **400** `{"error": "Invalid key format"}` (also 500 if `key` is `None`) |
| 7 | `394-395` | `check_salt_format_and_ethereum_address` | **400** `{"error": "Invalid salt format"}` |
| 8 | `398-399` | any of hash/key/account falsy | **400** `{"error": "Missing hash_to_verify, key, or account"}` |
| 9 | `403-418` | **difficulty floor** — see below | **401** `{"message": "Hash does not contain 'm=<D>'. Your memory_cost setting in your miner will be autoadjusted."}` |
| 10 | `421-435` | target present in `hash_to_verify[-87:]`; **XUNI additionally requires the server clock to be within 5 min of the hour** | **401** `{"message": "XUNI Submitted outside of proper time frame."}` |
| 11 | `437-442` | no target found at all | **401** `{"message": "Hash does not contain any of the valid targets ['XEN11'] ..."}` |
| 12 | `444-448` | `len > 150` | **401** (dead code — step 2 already nulled it) |
| 13 | `450-454` | `argon2.verify(key, hash_to_verify)` — recomputes Argon2 using the `m/t/p/salt` **embedded in the submitted string** | **401** `{"message": "Hash verification failed."}` on mismatch |
| 14 | `464-505` | INSERT into `blocks` (XEN11) or `xuni` (XUNI) | **400** `{"message": "Block already exists, continue"}` on `key` UNIQUE violation |
| 15 | `515` | success | **200** `{"message": "Hash verified successfully and block saved."}` |

**Step 7 in detail** (`gpage.py:281-328`, mirrored verbatim in `node_verify.py:76-123` and `utils/salt_unittest.py:17-63`):

```python
parts = hash_to_verify.split("$")
if len(parts) != 6:
    return False
salt = parts[4]

if pattern1.search(salt):            # r'WEVOMTAwODIwMjJYRU4'  → legacy global salt
    return True

if pattern2.fullmatch(salt):         # r'^[A-Za-z0-9+/]{27}$'
    salt += '=' * (4 - len(salt) % 4)
    decoded_str = base64.b64decode(salt).hex()
    if re.fullmatch(r'[0-9a-fA-F]{40}', decoded_str):
        if restore_eip55_address('0x' + decoded_str):   # Web3.to_checksum_address
            return True
return False
```

> **Notable gap:** the server never checks that the address encoded in the salt equals the submitted `account` field. Credit is attributed purely by the `account` field (`gpage.py:490-491`). Our queue must persist `account` alongside the hash so the pairing survives.

**Step 9 in detail** — this is the closest thing to an expiry (`gpage.py:401-418`):

```python
difficulty = get_difficulty()
submitted_difficulty = int(re.search(r'm=(\d+)', hash_to_verify).group(1))
...
if submitted_difficulty < int(difficulty):
    print("This Generates 401 for difficulty being too low", submitted_difficulty, int(difficulty))
    error_message = f"Hash does not contain 'm={difficulty}'. Your memory_cost setting in your miner will be autoadjusted."
    log_verification_failure(error_message, account)
    return jsonify({"message": error_message}), 401
```

The comparison is `<`, not `!=` — **over-difficulty hashes are always accepted.** This is the single most important fact for our offline-queue design. See §5.3.

**Persistence** (`gpage.py:464-505`) — batch insert with lock retry (`gpage.py:543-556`, 10 retries × 3 s):

```python
insert_query = '''INSERT INTO blocks (hash_to_verify, key, account) VALUES (?, ?, ?)'''
```

Only three columns are written. `block_id` is `AUTOINCREMENT` and `created_at` is `DEFAULT CURRENT_TIMESTAMP` — **both assigned by the server at INSERT time**.

---

### 2.4 `GET /getblocks/lastblock`

Returns the **last full page of 100 sealed block records** (`getblocks2.py:24-58`):

```python
c.execute("SELECT MAX(block_id) FROM blocks")
last_full_page_id = (last_id // 100) * 100
offset = last_full_page_id if last_id % 100 == 0 else last_full_page_id - 100
c.execute("SELECT * FROM blocks WHERE block_id > ? ORDER BY block_id ASC LIMIT 100", (offset,))
```

**Response 200:** JSON array of exactly 100 objects:
```json
[{"block_id": 63158401, "hash_to_verify": "$argon2id$...", "key": "0a1b...", "account": "0x...", "date": "2026-08-09 12:34:56"}, ...]
```

`date` is the server-side `created_at`.

---

### 2.5 `POST /send_pow`

Only fired after a **successful XEN11** submission (`miner.py:316-318`):

```python
if target == "XEN11" and found_valid_hash and response.status_code == 200:
    submit_pow(account, random_data, hashed_data)
```

`submit_pow` (`miner.py:160-222`) downloads the last 100-record page, re-verifies **every** record with `argon2.verify`, builds a merkle root, and posts:

```python
payload = {
    'account_address': account_address,   # our address
    'block_id': output_block_id,          # int(block_id / 100)
    'merkle_root': merkle_root,
    'key': key,                           # our winning key
    'hash_to_verify': hash_to_verify      # our winning hash
}
pow_response = requests.post('http://xenblocks.io:4446/send_pow', json=payload)
```

Handler (`indexing/merkleroot.py:12-34`) requires all five fields (**400** `{"message": "Invalid payload"}` otherwise) and does `REPLACE INTO merkleroot2 (block_id, merkleroot_hash, account, key, hash_to_verify)`. **200** `{"message": "POW Record stored successfully!"}`, **500** on DB error.

This is an *attestation* that the miner independently validated the last sealed block. It is not required for block credit — the XNM/X.BLK/XUNI balances in `make_cache.py:81-135` are computed purely from the `blocks`/`xuni`/`super_blocks` tables. It costs 100 full Argon2 verifications per submission, which is expensive.

> **For our miner:** `/send_pow` is decoupled from block credit and is inherently time-sensitive (it references *the current* last page). It should **not** be part of the offline queue — replaying a stale merkle root for an old page is pointless. Make it best-effort and fire-and-forget.

---

### 2.6 Other server endpoints (context, not miner-required)

| Endpoint | File:line | Purpose |
|---|---|---|
| `POST /validate` | `gpage.py:559-577` | Node consensus attestation: `{total_count, my_ethereum_address, last_block_id, last_block_hash}` → `consensus` table. Posted by `syncnode.py:62`. |
| `GET /get_block?key=<64hex>` | `gpage.py:331-364` | Look up one record by key; checks `blocks` then `xuni`. **Useful to us: lets a queue confirm whether a hash landed.** |
| `GET /total_blocks` | `gpage.py:211-224` | `{"total_blocks_top100": <max block_id>}` |
| `GET /total_blocks2?account=` | `gpage.py:632-644` | per-account block count |
| `GET /get_balance/<account>` | `gpage.py:173-190` | `total_blocks * 10` from `cache.db` |
| `GET /get_super_blocks/<account>` | `gpage.py:192-208` | superblock count |
| `GET /get_xuni_counts` | `gpage.py:120-143` | per-account XUNI counts |
| `GET /blockrate_per_day`, `/top_daily_block_miners` | `gpage.py:146-167`, `580-599` | leaderboards |
| `GET /latest_blockrate` | `gpage.py:602-628` | `{id, date, rate}` — current network block rate |
| `GET /hash_rate` | `gpage.py:228-245` | HTML page |
| `GET /leaderboard` | `gpage.py:169-171` | 302 → explorer |
| `GET /getallblocks2/<page>`, `/getblocks/all/<n>`, `/getallblocks/<page>`, `/download` | `getblocks2.py:60-204` | bulk sync (port 4447), used by `syncnode.py:206` |
| `GET /v1/leaderboard`, `/v1/leaderboard/<account>`, `/health` | `json_api/app/leaderboard/routes.py`, `json_api/app/main/routes.py` | modern JSON leaderboard API, port 5566 |
| JSON-RPC `POST /` | `rpc2.py:309-538` | Ethereum-shim RPC (`eth_getBalance`, `eth_call` for XUNI/X.BLK ERC-20 views, `eth_sendRawTransaction`) on port 5555 |

---

## 3. Block / Data Structures

### 3.1 The submitted record

What is durably stored per accepted find is only:

```sql
CREATE TABLE IF NOT EXISTS blocks (
    block_id       INTEGER PRIMARY KEY,          -- server AUTOINCREMENT
    hash_to_verify TEXT,
    key            TEXT UNIQUE,                  -- ← dedup key
    account        TEXT,
    created_at     DATETIME DEFAULT CURRENT_TIMESTAMP   -- ← SERVER-SET
);
```
(`p2pnode/sql.go:96-104`; identical `xuni` table at `p2pnode/sql.go:106-114`; same shape in `index_builder.py:10-18`, `utils/listener.py:29-35`, `gpage.py:20`)

**`key TEXT UNIQUE` on both `blocks` and `xuni`.** This gives us free idempotency — see §5.4.

### 3.2 The miner's local `Block` class is vestigial

`miner.py:98-124` defines a `Block` with `index/prev_hash/data/valid_hash/random_data/attempts/timestamp/hash`, chained in `blockchain[]` at `miner.py:339-370`. **None of it is transmitted.** `prev_hash` is passed into `mine_block` (`miner.py:231`) and never used. `Block.timestamp = time.time()` (`miner.py:106`) is purely local decoration. Also note the trailing block-append code at `miner.py:367-371` sits **outside** the `while` loop and is unreachable until the 20,000,000-block loop ends.

### 3.3 Block formation / sealing (100 records per block)

There is no per-find "block". Sealing is a **server-side batching of 100 accepted records**:

- `output_block_id = int(block_id / 100)` — `miner.py:200`, `merkleroot.py:53`
- `/getblocks/lastblock` returns exactly the last full 100-record page — `getblocks2.py:30-42`
- `syncnode.py:213-215` treats a page of `< 100` records as "all sealed blocks are current"
- `p2pnode/xenp2p.go` (`validateBlock`, ~`:317-343`) requires ≥100 records per block

### 3.4 Merkle root

Leaf construction is identical in all four implementations:

```python
verified_hashes.append(hash_value(str(block_id) + hash_to_verify + key + account))
```
(`miner.py:193`, `merkleroot.py:46`, `syncnode.py:118/123/227/232`)

where `hash_value(v) = sha256(v.encode()).hexdigest()`. XUNI records use `xuni_id` in place of `block_id` (`syncnode.py:112`, `:220`).

Tree building — pairwise SHA-256 over the **concatenated hex strings**, odd leaf duplicated (`miner.py:78-90`, `p2pnode/mtree.go:18-37`, `go/merkle_test.go`):

```python
def build_merkle_tree(elements, merkle_tree={}):
    if len(elements) == 1:
        return elements[0], merkle_tree
    new_elements = []
    for i in range(0, len(elements), 2):
        left  = elements[i]
        right = elements[i + 1] if i + 1 < len(elements) else left
        new_hash = hash_value(left + right)
        merkle_tree[new_hash] = {'left': left, 'right': right}
        new_elements.append(new_hash)
    return build_merkle_tree(new_elements, merkle_tree)
```

(Note the classic mutable-default-argument bug on `merkle_tree={}` — harmless here since only the root is used.)

Chained blockchain record (`syncnode.py:159-165`, `p2pnode/sql.go:20-28`):

```sql
CREATE TABLE blockchain (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    prev_hash TEXT, merkle_root TEXT, records_json TEXT, block_hash TEXT)
```

with `block_hash = sha256(str(prev_hash) + str(merkle_root))` (`syncnode.py:240-241`). **The block hash does not commit to the timestamp** — timestamps are not part of the chain's integrity at all.

### 3.5 Block time expectations

- Target: **70 XEN11 blocks per minute** ≈ **0.86 s/block** network-wide (`manage_difficulty2.py:48-57`). The README claims ~1 block/second — consistent.
- Sealed block (100 records) ≈ **~86 seconds**.
- Difficulty re-evaluated every **300 s** (`manage_difficulty2.py:92`), block rate sampled every **60 s** over the last 300 blocks (`indexing/rate_daemon.py:25`, `:55`).
- Client difficulty poll: every **60 s** (`miner.py:136`).

### 3.6 Reward accounting / epochs

`make_cache.py:81-135` — XNM issuance halves by epoch:

```sql
CASE WHEN block_id >= 63158400 THEN 3
     WHEN block_id >  29818420 THEN 2
     ELSE 1 END AS epoch
SUM(blocks_per_epoch * POWER(10, 19) / POWER(2, epoch - 1)) AS xnm
```

So XNM per block = `10 XNM / 2^(epoch-1)`. `xblk = super_blocks * 1e18`, `xuni = total_xuni * 1e18`. Rebuilt every 300 s (`make_cache.py:158-161`). Superblocks recounted every 300 s (`make_superblocks.py:72-75`).

> **Implication:** a queued block resubmitted late gets whatever `block_id` it receives at insert time, and therefore whatever epoch that `block_id` falls in. Crossing an epoch boundary during an outage halves the reward for queued blocks. Epoch boundaries are ~34M blocks apart (~1 year at 70/min), so this is a negligible edge case, but it is the one place where lateness costs value.

---

## 4. Failure Modes — What Happens Today When the Server Is Down

### 4.1 `/verify` unreachable → **the miner crashes and the work is lost**

`miner.py:306-326`:

```python
    max_retries = 2
    retries = 0

    while retries <= max_retries:
        # Make the POST request
        response = requests.post('http://xenblocks.io/verify', json=payload)   # ← line 311, NO try/except

        # Print the HTTP status code
        print("HTTP Status Code:", response.status_code)

        if target == "XEN11" and found_valid_hash and response.status_code == 200:
            submit_pow(account, random_data, hashed_data)

        if response.status_code != 500:      # If status code is not 500, break the loop
            print("Server Response:", response.json())
            break

        retries += 1
        print(f"Retrying... ({retries}/{max_retries})")
        time.sleep(10)
```

- **`miner.py:311` has no exception handling and no timeout.** A `ConnectionError` / `ConnectTimeout` / DNS failure propagates out of `mine_block`, out of the `while i <= num_blocks_to_mine` loop (`miner.py:353-365`), and **terminates the process**. Contrast with `submit_pow`, which *does* guard its GET (`miner.py:164-170`).
- No timeout means a hung TCP connection blocks the miner **indefinitely** — the mining thread is the main thread, so hashing stops entirely.
- The retry loop only covers **HTTP 500**. Every other non-200 (`400`, `401`) `break`s immediately with **no retry and no persistence** — the find is discarded.
- Retry backoff is a fixed `time.sleep(10)`, max 2 retries → 20 s total. No jitter, no exponential backoff.
- Bug: the `print("Server Response:", response.json())` at `miner.py:330-333` is inside the `while` but **after** `time.sleep(10)`, so it re-prints the *previous* response body; and if the server returns 500 with a non-JSON body the exception is swallowed.

### 4.2 The one thing that *is* durable today

`miner.py:298-302` — written **before** the POST:

```python
    # Append the string to a log file
    log_file = 'log_blocks.log'  # replace with your log file's path

    with open(log_file, 'a') as file:  # 'a' means append mode
        file.write(json_dumps(payload) + '\n')
```

Every found hash is appended as one JSON line to `log_blocks.log` prior to submission. This is an accidental, append-only, unacknowledged write-ahead log. It is never read back, never pruned, and there is no replay tooling anywhere in the repo. **It is nonetheless proof that the payload is fully self-contained and safe to persist — this is the natural seed for our offline queue.**

### 4.3 `/difficulty` unreachable → graceful

`miner.py:139-147` catches broadly and returns the last-known `memory_cost`. Mining continues at the last known difficulty. This is the correct behaviour and the only place the reference miner degrades gracefully. (Subject to the str/int bug in §1.3.)

### 4.4 `getblocks/lastblock` unreachable → graceful

`miner.py:163-175` — guarded with `timeout=10` and a `RequestException` handler, returns `None`. Non-fatal; only the merkle attestation is skipped.

### 4.5 `/send_pow` unreachable → **crashes**

`miner.py:212` — `requests.post('http://xenblocks.io:4446/send_pow', json=payload)` has no try/except and no timeout. Same failure mode as §4.1. Since `submit_pow` is invoked at `miner.py:318` *after* a successful `/verify`, a `:4446` outage kills the miner even though the block was already credited.

### 4.6 Summary table

| Condition | Reference behaviour | Work lost? |
|---|---|---|
| `/verify` connection refused / DNS fail / timeout | **Uncaught exception → process death** | Yes (only in `log_blocks.log`) |
| `/verify` hangs (no socket timeout set) | **Blocks forever, hashing stops** | Effectively yes |
| `/verify` → 500 | 2 retries, 10 s apart, then give up | Yes after 3 attempts |
| `/verify` → 401 (difficulty too low / bad target) | Immediate `break`, no retry | Yes |
| `/verify` → 400 (duplicate key) | Immediate `break` | N/A — already credited |
| `/difficulty` down | Caught; keeps last difficulty | No |
| `/getblocks/lastblock` down | Caught; skips merkle attestation | No |
| `:4446/send_pow` down | **Uncaught exception → process death** | Block already credited, but miner dies |

---

## 5. Offline-Queue Design — The Critical Findings

### 5.1 Are timestamps client-set or server-set? → **Entirely server-set**

**Exhaustive check. There is no timestamp field in the `/verify` payload.** The miner constructs the payload at `miner.py:289-296` with exactly six keys — `hash_to_verify`, `key`, `account`, `attempts`, `hashes_per_second`, `worker`. None is temporal.

On the server:
- `gpage.py:458` computes `timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')` — but its **only** consumer, the `account_attempts` INSERT, **is commented out** (`gpage.py:499-500`). The variable is dead.
- The actual INSERT writes **three columns only** (`gpage.py:490-491`): `hash_to_verify, key, account`.
- `created_at` comes from `DEFAULT CURRENT_TIMESTAMP` in the schema (`p2pnode/sql.go:102`, `:112`; `index_builder.py:16`; `utils/listener.py:34`; `utils/listener2.py:113`; `utils/listener3.py:108`).

**Therefore a resubmitted hash is dated at resubmission time, and there is no mechanism by which the server could know or care when it was actually mined.** This is exactly what we want: nothing to forge, nothing to reconcile.

Downstream systems that *do* touch time — and confirmation that none of them validate:
- `indexing/rate_daemon.py:25-40` uses `created_at` only to compute the aggregate block rate → this is the **one real side-effect of a bulk flush**: dumping a large backlog spikes the measured rate, which pushes difficulty up by +1000. Drain politely (§5.6).
- `utils/broadcaster.py:127-132` stamps a *send-time* millisecond timestamp into the websocket wire format `block_id|hash_to_verify|key|account|created_at|timestamp` — stamped by the broadcaster, not the miner.
- `utils/listener.py:76-84`, `listener2.py:155-165`, `listener3.py:150-160` compute `timestamp_diff = local_timestamp - timestamp_int` — **logged and echoed only, never compared to a threshold, never used to reject.** Confirmed: there is no `if timestamp_diff > ...` in any listener.
- `p2pnode/` (Go): **no timestamp validation of any kind.** `HashRecord.CreatedAt`, `Block.Timestamp`, and `RangeRecord.Ts` are copied through DB→gossip→DB verbatim and never parsed or compared against `time.Now()`. The only time parsing in the package (`p2pnode/tools.go:260-263`) is commented out. Deduplication is by primary key / `ON CONFLICT DO NOTHING` only — no replay window, no nonce, no monotonic-time gate.
- `p2pnode/xenp2p.go` block validation recomputes the merkle root and checks `prevBlock.BlockHash == block.PrevHash`, but never touches `Timestamp`.

### 5.2 Is there a submission deadline or expiry? → **No, for XEN11. Yes, for XUNI.**

**XEN11 / XNM — no expiry whatsoever.** The server says so in a comment (`gpage.py:481`):

```python
            elif is_xen11_present:  # no time restrictions for XEN11
```

There is no max-age, no block-height binding, no prev-hash binding, no nonce window, no epoch check at verify time. The proof is fully self-contained: `argon2id(key, salt=account_bytes, m,t,p)` either produces `hash_to_verify` or it does not, and that fact is time-invariant.

**XUNI — hard, unqueueable window.** `gpage.py:429-435`:

```python
    if re.search('XUNI[0-9]', hash_to_verify[-87:]) is not None:
        found = True
        print("Found Target: XUNI[0-9]")
        if not is_within_five_minutes_of_hour():
            error_message = f"XUNI Submitted outside of proper time frame."
            return jsonify({"message": error_message}), 401
```

with (`gpage.py:36-40`):

```python
def is_within_five_minutes_of_hour():
    timestamp = datetime.now()
    minutes = timestamp.minute
    return 0 <= minutes < 5 or 55 <= minutes < 60
```

This is evaluated against the **server's wall clock at receive time**, on `datetime.now()` (server-local timezone, not UTC). It says nothing about when the hash was mined. Consequences both ways:

- A XUNI found at 11:58 and queued through an outage until 12:30 is **rejected (401)**.
- Conversely, a XUNI found at 11:58 and flushed at 12:57 is **accepted** — there is no proof of mine time, so the window is trivially bypassable by holding. (We should not build on this; note it only as evidence that no anti-replay exists.)
- A second, redundant window check sits at `gpage.py:469` and the `else` branch at `gpage.py:496-497` returns 401 `"XUNI found outside of time window"` — unreachable given the earlier guard.

### 5.3 The real constraint: the difficulty floor

`gpage.py:412`:

```python
    if submitted_difficulty < int(difficulty):
```

where `submitted_difficulty` is parsed from `m=(\d+)` **inside the stored hash string** (`gpage.py:404`) and `difficulty` is the **current** network difficulty read fresh from `diff2.chain` on every request (`gpage.py:403` → `gpage.py:67-76`).

**A queued hash is accepted iff its baked-in `m=` is ≥ the network difficulty at the moment of flush.** Since `m=` is immutable once mined, a rising difficulty is a hard, irreversible expiry for the backlog.

Quantifying the risk from `manage_difficulty2.py:59-68`:

| Property | Value |
|---|---|
| Adjustment interval | 300 s |
| Max increase per tick | **+1000 KiB** |
| Decrease per tick | −2000 KiB |
| Floor | 100 |
| Trigger | measured rate outside `[56, 84]` blocks/min (or `[61, 84]` outside the XUNI window) |

Worst case, difficulty climbs **+1000 KiB per 5 minutes = +12,000 KiB/hour**. During an outage, though, network hashrate *drops* (miners crash per §4.1), so the measured rate falls and difficulty is far more likely to **decrease** by 2000/tick — which strictly *helps* our backlog.

**Mitigation strategy for our miner — mine with headroom.** Set `memory_cost = current_network_difficulty + margin`. Because the check is `<` and not `!=`, over-difficulty hashes are always valid. A margin of `N × 1000` KiB guarantees survival of `N × 5` minutes of monotonic difficulty increase. The cost is proportional memory/time per hash, so this is a tunable knob: e.g. `margin = 3000` (~15 min of worst-case protection) costs ~4% more memory at a difficulty of 80,000.

**Secondary mitigation — flush ordering is irrelevant to acceptance** (every queued item is compared against the same current difficulty), but **flush highest-`m=` last** is still wise: if difficulty is climbing during the drain, the low-`m` items expire first, so drain **ascending by `m=`** to lose the fewest.

**Detection:** a 401 whose body matches `Hash does not contain 'm='` is the *difficulty-expired* signal. Our queue should parse it, mark the item dead (do **not** retry forever), and surface a counter. The same response also tells us the current difficulty via the interpolated `m={difficulty}` in the message string — free difficulty discovery on failure.

### 5.4 Idempotency and replay safety — free from `key UNIQUE`

Both `blocks` and `xuni` declare `key TEXT UNIQUE` (`p2pnode/sql.go:100`, `:110`; `gpage.py:20`), and `gpage.py:507-510` translates the violation into a distinct response:

```python
        except sqlite3.IntegrityError as e:
            error_message = e.args[0] if e.args else "Unknown IntegrityError"
            print(f"Error: {error_message} ", hash_to_verify, key, account)
            return jsonify({"message": f"Block already exists, continue"}), 400
```

**This is exactly the acknowledgement primitive an offline queue needs.** Rules for our resubmitter:

| Response | Meaning | Queue action |
|---|---|---|
| `200` `"Hash verified successfully and block saved."` | accepted | **dequeue (success)** |
| `400` `"Block already exists, continue"` | already credited (previous attempt landed, ack was lost) | **dequeue (success)** |
| `401` `"Hash does not contain 'm=...'"` | difficulty expired | **dequeue (dead)**, increment expired counter |
| `401` `"XUNI Submitted outside of proper time frame."` | wrong hour-window | **keep**, retry at the next `:55–:05` window |
| `401` `"Hash verification failed."` | corrupt stored record | **dequeue (dead)**, log loudly — indicates local storage corruption |
| `400` `"Invalid key format"` / `"Invalid salt format"` / `"Missing ..."` | malformed record | **dequeue (dead)**, log |
| `500` / connection error / timeout | server down | **keep**, exponential backoff |

Because duplicates are safely rejected with an unambiguous message, our queue can be **at-least-once** rather than exactly-once — we never need to risk dropping a record to avoid a double-credit.

### 5.5 What the queued record must contain

Minimal sufficient set (everything else is decorative per §2.3):

```json
{
  "hash_to_verify": "$argon2id$v=19$m=80000,t=1,p=1$<salt27>$<digest86>",
  "key": "<64 hex chars>",
  "account": "0x...",
  "found_at": 1754749200.123,          // OURS — local only, never sent
  "kind": "XEN11" | "XUNI",            // OURS — drives XUNI window scheduling
  "m": 80000,                          // OURS — parsed from the hash, drives ordering/expiry
  "attempts": 12345,                   // optional, ignored by server
  "hashes_per_second": 41.2,           // optional, ignored by server
  "worker": "01"                       // optional; server reads `worker_id` and requires str len<=3
}
```

`found_at` and `kind` and `m` are **for our own bookkeeping only**. Sending extra JSON keys is harmless — `gpage.py:369` uses `data.get(...)` and ignores unknown fields.

The record is fully self-validating offline: we can re-run `argon2.verify(key, hash_to_verify)` locally before flushing to catch storage corruption without burning a server round-trip.

### 5.6 Operational cautions for a bulk flush

1. **Backlog flush inflates the measured block rate.** `indexing/rate_daemon.py:25-44` computes rate over the last 300 blocks' `created_at`; `manage_difficulty2.py` then raises difficulty +1000 if the rate exceeds 84/min. A large instantaneous dump raises difficulty for the whole network and, worse, can push the floor above our *own* remaining queued items' `m=`. **Throttle the drain** — a modest rate (e.g. a few per second) is both polite and self-protective.
2. **No rate limiting exists in the code** on `/verify` (no `limit_req` in `system/etc/nginx/nginx.conf`; the port-80 vhost that serves `/verify` is not even in the repo). Do not take this as licence — the SQLite writer serialises behind a 10-retry × 3 s lock loop (`gpage.py:543-556`), so hammering it degrades everyone.
3. **Set timeouts on every request.** The reference sets one only on the `getblocks` GET (`miner.py:166`). Ours must set connect+read timeouts on all four endpoints.
4. **Never let an HTTP failure kill the hashing loop.** Submission must be on a separate thread/task from mining, fed by the durable queue.
5. **`/send_pow` should not be queued** (§2.5) — it references the *current* last page. Fire it best-effort after a live 200, skip it entirely when draining a backlog.
6. **Use `GET /get_block?key=<key>`** (`gpage.py:331-364`) as an out-of-band reconciliation tool: it returns 200 with the stored row if the key landed, 404 otherwise. Useful for auditing a queue after an ambiguous outage, and for a `--verify-queue` mode.
7. **Server clock, not ours, gates XUNI.** Do not schedule XUNI flushes off local time alone; the server uses `datetime.now()` in its own (unknown) timezone. Prefer a probe-and-learn approach: attempt a XUNI flush, and use the 401 vs 200 to calibrate the actual server window.
8. **Epoch boundaries** (`make_cache.py:93-97`) are the only place lateness costs value — a block credited after `block_id` crosses 63,158,400 (epoch 3) earns half. Boundaries are ~1 year apart; log a warning if the queue is held across one.

### 5.7 Feasibility verdict

| Question | Answer | Evidence |
|---|---|---|
| Does the payload carry a client timestamp? | **No** | `miner.py:289-296` |
| Does the server record a client-claimed time? | **No** — `DEFAULT CURRENT_TIMESTAMP` at INSERT | `gpage.py:490-491`, `p2pnode/sql.go:102` |
| Does the server compare submit time vs claimed time? | **No such comparison exists anywhere** | grepped `gpage.py`, `node_verify.py`, `p2pnode/*.go`, `utils/listener*.py` |
| Is there a max age / expiry for XEN11? | **No** | `gpage.py:481` — *"no time restrictions for XEN11"* |
| Is there any expiry-like mechanic? | **Yes — the difficulty floor** | `gpage.py:412` |
| Are duplicate submissions safe? | **Yes** — `key UNIQUE` → 400 `"Block already exists, continue"` | `gpage.py:507-510`, `p2pnode/sql.go:100` |
| Can XUNI be queued across an outage? | **No, not reliably** — server-clock hourly window | `gpage.py:429-435` |
| Is delayed resubmission of XEN11 feasible? | **Yes, with a difficulty-headroom margin** | §5.3 |

---

## 6. Repo Map

### Root — miner and production servers

| Path | Description |
|---|---|
| `miner.py` | **The reference CPU miner.** Argon2id brute-force loop, difficulty polling thread, `/verify` submission, merkle `/send_pow`. |
| `config.conf` | Miner settings: `difficulty` (Argon2 `t`), `memory_cost` (bootstrap `m`), `cores` (`p`), `account`, `last_block_url`. |
| `gpage.py` | **The production verification/API server.** Owns `POST /verify` (the endpoint miners hit), `/difficulty`, `/validate`, `/get_block`, balance and leaderboard reads. |
| `node_verify.py` | Stripped standalone verifier (port 8888) — same validation logic as `gpage.py` but **no DB writes**. Reference/test node; `get_difficulty()` hardcoded to 8. |
| `getblocks2.py` | Block-serving API (port 4447): `/getblocks/lastblock`, `/getblocks/all/<n>`, `/getallblocks2/<page>`, `/total_blocks`, `/download`. |
| `merkleroot.py` | Standalone CLI: fetch last 100 blocks, argon2-verify each, build merkle root, POST `/send_pow`. Same logic as `miner.submit_pow`. |
| `manage_difficulty2.py` | **Difficulty controller.** Every 300 s: target 70 blocks/min ±20%, +1000/−2000 steps, floor 100, writes `diff2.chain`. |
| `make_superblocks.py` | Every 300 s: recount superblocks (digest body ≥ 50 uppercase) into `super_blocks`. |
| `make_cache.py` | Every 300 s: rebuild `cache.db → cache_table` — ranks, XNM (epoch-halved), X.BLK, XUNI, Solana address join. |
| `syncnode.py` | Independent node syncer: pulls pages, argon2-verifies, rebuilds merkle roots, chains blocks into `blockchain.db`, POSTs `/validate`. |
| `index_builder.py` | One-shot: explode `blockchain.records_json` back into a flat `blocks` table. |
| `rpc_server.py` | Early Ethereum JSON-RPC shim (port 5555) reading balances from `blocks.db`. Superseded by `rpc2.py`. |
| `rpc2.py` | **Production** Ethereum JSON-RPC shim: `eth_getBalance`, `eth_call` ERC-20 views for XUNI (`0x…00002`) and X.BLK (`0x…00001`), `eth_sendRawTransaction` → `transactions` table. |
| `test_miner.py` | `miner.py` pointed at `localhost:8888`, target `XEN1`. Local dev harness. |
| `miner_test.py` | Older `miner.py` fork: target `XEN`, superblock rule "≥65 capitals after digit-strip", no submission block. **Obsolete — do not use as a spec.** |
| `matrix.py` | Matrix-rain terminal toy that renders the latest `block_id`. |
| `bench.go` | Benchmarks Ethereum raw-tx ECDSA sender recovery. |
| `ptrie_unittest.py` | Patricia-trie unit test. |
| `requirements.txt` | `passlib, requests, tqdm, argon2_cffi, web3, flask*, gunicorn, gevent, ethereum, eth_utils`. |
| `README.md` | Short install/run guide; states Argon2ID, GPU/ASIC-resistant, ~1 block/sec target. |

### `p2pnode/` — Go libp2p gossip node

| Path | Description |
|---|---|
| `xenp2p.go` | Main node: GossipSub topics `block_height`, `get`, `data`, `new_hash`, `new_xuni`, `shift`, `get_raw`, `control`; block validation (merkle + prev-hash chain); difficulty estimation from hash-id arrival deltas. **No Argon2 verification, no timestamp validation.** |
| `setup.go` | libp2p host, QUIC transport, Kademlia DHT bootstrap, topic subscription, optional Redis. |
| `sql.go` | All SQL constants — canonical `blocks` / `xuni` / `blockchain` / `control` schemas. `key TEXT UNIQUE`, `created_at DEFAULT CURRENT_TIMESTAMP`. |
| `db.go` | DB accessors. (Contains a column-order bug: scans `Id, CreatedAt, Key, HashToVerify, Account` against SQL selecting `block_id, hash_to_verify, key, account, created_at`.) |
| `mtree.go` | SHA-256 merkle tree, odd leaf duplicated — mirrors `miner.build_merkle_tree`. |
| `node.go` | Role enum: `supernode`, `relay`, `miner`, `validator`, `bootstrap`, `rpc`. |
| `rpc.go` | Fiber HTTP server on `:3333` — `/status`, `/pubsub/:topic`, `/dht`. |
| `tools.go` | Helpers; the package's only time-parsing code, and it is commented out. |

### `json_api/` — modern leaderboard API (port 5566)

| Path | Description |
|---|---|
| `app/__init__.py` | Flask app factory, three SQLAlchemy binds (`blocks`, `cache`, `difficulty`). |
| `app/leaderboard/routes.py` | `GET /leaderboard` (302→explorer), `GET /v1/leaderboard`, `GET /v1/leaderboard/<account>`. |
| `app/leaderboard/service.py` | Builds `{totalHashRate, totalMiners, totalBlocks, difficulty, totalXnm/Xuni/Xblk, miners[]}`; EIP-55-checksums accounts. |
| `app/main/routes.py` | `GET /health`. |
| `app/models/*.py` | `cache_table`, `blockrate`, `miners`, `blocks` ORM models. |
| `config.py`, `gunicore_config.py` | Port 5566, 30 gevent workers. |

### `indexing/` — server-side daemons

| Path | Description |
|---|---|
| `rate_daemon.py` | Every 60 s: block rate over last 300 blocks' `created_at` → `difficulty.db → blockrate`. Feeds the difficulty controller. |
| `manage_difficulty.py` | Original difficulty controller (target 60/min, ±100 steps). Superseded by root `manage_difficulty2.py`. |
| `merkleroot.py` | **The `/send_pow` server** — `REPLACE INTO merkleroot2 (block_id, merkleroot_hash, account, key, hash_to_verify)`. |
| `getblocks.py` | Earlier block-serving API. |
| `count_miners.py` | Counts unique miner IPs by grepping nginx `access.log` for `verify`. |
| `count_consensus.py`, `check_rate.py`, `check_seq.py`, `block_rate_per_account.py` | Ad-hoc analytics / integrity checks. |

### `utils/` — websocket pipeline, state, tooling

| Path | Description |
|---|---|
| `broadcaster.py` | WebSocket fan-out on `:6668`. Polls `blocks.db` every 0.5 s, emits zlib-compressed `block_id\|hash_to_verify\|key\|account\|created_at\|timestamp`. Ingests client replies into Redis; **never validates `time_diff`**. |
| `listener.py` / `listener2.py` / `listener3.py` | WebSocket clients that mirror records into a local `blocks.db`. Compute `timestamp_diff` and **only log it**. Argon2 verification is commented out on the hot path. |
| `voter.py` | WebSocket client with Argon2 verification and a 100-block deque. |
| `go/main.go`, `go/utils.go` | Go WS client, verifies Argon2id **locally** by re-deriving `argon2.IDKey` from the parsed `$m=..,t=..,p=..$salt$` string. Good reference for a non-passlib verifier. |
| `go_listener/main.go` | Go WS client that delegates verification to `http://209.124.84.6:5011/verify`; also runs libp2p GossipSub. |
| `make_state.py`, `make_state3.py`, `make_index.py`, `account_manager.py` | Build `account_balances` state; decompose hashes into `m/t/p/salt/key` columns; Patricia-trie account store. |
| `gen_balances.py`, `get_super_blocks.py`, `check_superblocks.py` | Balance and superblock reporting. |
| `verify_proofs.py` | Standalone Argon2 hash regeneration from stored `m/t/p/salt/key` — useful reference for offline re-verification. |
| `salt_unittest.py` | **Best single reference for the salt rules** — the exact `check_salt_format_and_ethereum_address` function plus five real sample hashes (both legacy and per-account salts). |
| `watchBlocks.py` | Web3 listener for the `NewHash` event on the X1 testnet `BlockStorage` contract. |
| `engine.sol`, `build/contracts/BlockStorage.json` | On-chain block-storage contract + ABI. |
| `bench.py`, `run_node.go` | RLP decode benchmark; libp2p timestamp-gossip demo. |

### Other

| Path | Description |
|---|---|
| `go/merkle_test.go`, `tests/merkle_unittest.py` | Merkle tree unit tests (Go and Python). |
| `dev/ptrie_db_*.py`, `dev/bench_mempool*.go` | Patricia-trie storage experiments (RLP/zlib) and mempool benchmarks. |
| `system/etc/nginx/nginx.conf` | Production nginx: `xenblocks.io:443` (explorer, `/v1`, `/validate`, `/leaderboard` → 5567) and `:5556` → RPC 5555. **Note: the port-80 vhost serving `/verify` is not in the repo, and no `limit_req` is configured anywhere.** |
| `system/etc/systemd/system/*.service` | Units for `xenblocks-json-api`, `xenblocks-make-cache`, `xenblocks-make-superblocks`. |
| `vast/vash.sh` | Vast.ai GPU-node bootstrap; clones `shanhaicoder/XENGPUMiner` (the GPU miner lives in a separate repo). |
| `docs/` | `xenblocks_whitepaper.v0.1.pdf`, `XENIUM_BLOCKCHAIN.pdf`, `xone_tokenomics.pdf`, `Concurrency_in_world_state.pdf`, `x1node.json`. |

---

## 7. Concrete Recommendations for Our Miner

1. **Persist before submitting.** Write the record to a durable local store (SQLite with a `status` column beats an append-only log) *before* the HTTP call — the reference already half-does this at `miner.py:298-302`.
2. **Decouple mining from submitting.** Hashing on one thread, a submitter draining the queue on another. Never let `requests` exceptions or hangs reach the mining loop — the #1 reference bug (§4.1).
3. **Timeouts on everything**, with exponential backoff + jitter for connection errors and 500s.
4. **Mine with difficulty headroom.** `memory_cost = network_difficulty + margin` (e.g. `+3000`) — the check is `<`, so over-difficulty always passes, and every 1000 KiB of margin buys ~5 minutes of outage tolerance (§5.3).
5. **Classify responses per the table in §5.4.** Treat 400 `"Block already exists, continue"` as **success**, and 401 `"Hash does not contain 'm='"` as **permanently dead** — do not retry either.
6. **Drain ascending by `m=`**, throttled, so a backlog flush neither expires our own low-margin items nor spikes network difficulty (§5.6).
7. **Handle XUNI separately.** Only flush XUNI during a live `:55–:05` window; if the queue holds XUNI past a window, retry at the next one rather than burning attempts. Consider making XUNI opt-in given its unqueueable nature.
8. **Normalise difficulty to `int`** on both the success and failure paths of the fetch, avoiding the reference's str/int type churn (§1.3).
9. **Keep `/send_pow` out of the queue** — best-effort only, guarded, after a live 200 (§2.5).
10. **Ship a `--verify-queue` mode** that re-runs `argon2.verify(key, hash_to_verify)` locally and cross-checks landed records via `GET /get_block?key=` (§5.6).
