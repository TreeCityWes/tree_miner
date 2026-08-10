# 07 — External model reviews: what they found and what we adopted

Three independent reviews of the TreeMiner plan were produced by other models and are
preserved verbatim in `docs/reviews/`:

| Doc | Reviewer |
|---|---|
| `reviews/KIMMY-PLAN.md` | Kimmy |
| `reviews/GROK-RECOMMENDATIONS.md` | Grok |
| `reviews/SOL-PLAN.md` | Sol |

The authoritative record of every adopted amendment is **`treeminer/PLAN.md` §10** (each
amendment credits its source). This doc is the short narrative version.

## Findings that changed the design

- **Stale-difficulty silent drop (Kimmy + Sol; the single best catch).** Upstream
  `main.cpp:371-381` re-hashed with `globalDifficulty` at submit time and silently
  `return`ed — dropping the find — if difficulty had ticked since the GPU batch started.
  My own first-hand review missed it (I started reading two lines past the `return`).
  Fix: immutable `FoundPayload` capture — the PHC string is assembled exactly once from
  the batch's actual parameters (`treeminer/src/treeminer/PhcAssembler.h`), and both CPU
  re-hashes were deleted.
- **`TransportResult` shape (Sol §8).** The submitter's transport interface
  (`src/submit/ITransport.h`) carries `transport_ok / http_status / body / retry_after /
  date_header / error` exactly as Sol proposed, which made the classifier purely
  data-driven and unit-testable.
- **Backoff must survive restarts (Sol §6).** `recoverOnStartup()` deliberately preserves
  persisted `next_attempt_at`; only in-flight `Submitting` leftovers reset to `Pending`.
- **Server clock offset from HTTP `Date` headers (Sol §7).** Adopted for the XUNI window
  estimate rather than trusting the local clock.
- **Confirmation-aware acks.** The reviews' skepticism about trusting HTTP 200 reinforced
  the `/get_block` reconciliation path; the server's lying-200 (insert failure still
  returns 200, `gpage.py:492-494,515`) is now detected and re-driven. Validated live.

## Claims we checked and rejected or corrected

- **"Four-warps-per-block was already tried and rejected upstream" (Sol).** Misreading:
  the rejected upstream experiment was a `__launch_bounds__(THREADS_PER_LANE, 4)` register
  hint, a different lever entirely. Multi-warp block packing remains a live Phase-2
  candidate pending Nsight evidence.
- **Device-side finalize failed upstream (Sol).** Confirmed in the upstream experiment
  ledger (buffer-lifetime failure) — kept on the candidate list but flagged high-risk.

## Where the rest landed

The richer find state machine (parking, quarantine, XUNI window budgets), adaptive drain
pacing, outage difficulty-margin presets, and keygen hardening were converged on by the
reviews collectively and are specified in `treeminer/PLAN.md` §10, with implementation in
`treeminer/src/journal/` and `treeminer/src/submit/`.
