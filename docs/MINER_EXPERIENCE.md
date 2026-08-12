# TreeMiner Experience Direction

## Product Position

TreeMiner should feel like an operator console for cryptographic infrastructure,
not a stream of implementation details. The terminal remains the fastest view;
the local web console adds history, inspection, and safe configuration workflows.

The visual language follows HashHead: near-black surfaces, cyan and signal-green
telemetry, monospace metadata, compact numbered sections, and restrained edge
light. Warning colors have operational meaning and are not decorative.

## Event Language

User-facing events answer three questions: what happened, where the find is, and
whether action is required.

| Internal state | Operator language |
| --- | --- |
| Durable journal append | `Find secured locally` |
| Pending submission | `Waiting for delivery` |
| Circuit breaker open | `Network offline - mining continues` |
| HTTP accepted, not reconciled | `Accepted - confirming on chain` |
| Reconciled through `/get_block` | `Accepted and confirmed` |
| Retryable transport failure | `Delivery paused - find remains secured` |
| Parked for eligibility | `Held for the next eligible window` |
| Permanent invalid response | `Rejected - review required` |
| Journal write failure | `CRITICAL - find could not be secured` |

Detailed file logs may retain identifiers, HTTP status codes, and classifier
reasons. The live terminal should show those only when they help an operator make
a decision.

## Local Console

The first dashboard release is served by the miner at
`http://127.0.0.1:42069`. It is self-contained except for one bundled visual
asset and remains available when the Xenblocks endpoint is offline.

Initial scope:

- GPU, CPU, and combined throughput.
- Difficulty, uptime, session finds, and separate XNM/XUNI secured queues.
- Per-device CUDA stream telemetry.
- Network and last-submission state.
- A sanitized runtime snapshot export.

## Terminal Presentation Mode

The miner has two terminal surfaces:

- `--display logs` preserves the scrolling event stream and is the default for
  unattended compatibility.
- `--display terminal` opens a fixed full-screen operator console with an
  animated decorative hash-rain rail on wide terminals.
- `--display prompt` asks the operator to choose between those views at boot.

The presentation terminal shows raw throughput, difficulty-adjusted work rate,
GPU and CPU allocation, delivery state, separate queue depths, and a bounded
recent-event feed. It uses the terminal alternate screen so Ctrl-C restores the
original shell and scrollback. The animated field is automatically omitted on
narrow terminals where it would compete with operational data.

The server binds to localhost by default. Remote use should go through an SSH
tunnel until configurable binding and bearer authentication are implemented.

## Feature Priorities

1. Recent activity feed with sanitized find lifecycle events.
2. Outage duration, next probe time, and confirmation backlog.
3. Twenty-four-hour in-memory history for hashrate, difficulty, queues, and
   connectivity.
4. Versioned effective configuration with source provenance: default, saved, or
   command-line override.
5. Sanitized configuration export.
6. Restore with schema validation, migration, previewed diff, atomic write,
   rollback, and explicit restart.
7. Diagnostics bundle for support: hardware, version, effective plan, counters,
   and classified events without keys, hashes, tokens, or credential-bearing
   URLs.

Configuration restore is intentionally deferred. The current `config.txt` is
unversioned, non-atomic, and does not represent CUDA streams, CPU workers, device
selection, or batch settings. Exporting that file would create false confidence
that a rig can be reproduced.

## Acceptance Rules

- The console must never block hashing, submission, or journal writes.
- Dashboard labels must distinguish finds discovered from submissions confirmed.
- The page must remain truthful and usable while the upstream server is down.
- No private keys, journal keys, full hashes, bearer tokens, or raw credentialed
  endpoints may be exposed.
- Write controls require authentication, validation, atomic persistence, and an
  audit event before they can ship.
