#pragma once
// Abstract journal contract. The submitter depends on this interface only; the SQLite
// implementation lives in src/journal/. Owned by the integration lead — do not modify.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Types.h"

namespace treeminer {

class IFindJournal {
public:
    virtual ~IFindJournal() = default;

    // Journal-first invariant: returns only after INSERT is durably committed (WAL, FULL).
    // Duplicate key returns the existing record's id (idempotent local capture).
    virtual std::int64_t append(const FoundPayload& payload) = 0;

    // Oldest-first eligible work for the drain scheduler. Eligibility: status Pending and
    // next_attempt_at is null or <= now_utc. Ordering nuances (XUNI preemption, ascending-m
    // under rising difficulty) are the scheduler's job, applied over this result.
    virtual std::vector<FindRecord> fetchEligible(const std::string& now_utc,
                                                  std::size_t limit) = 0;

    // As fetchEligible, restricted to one kind. Exists because a single mixed LIMIT slice
    // lets one kind starve the other: XUNI mined outside a window is Pending but not
    // submittable, so a slice full of it returns nothing selectable while XEN11 waits behind
    // it — and symmetrically, a deep XEN11 backlog can hide a time-critical XUNI. The
    // scheduler asks per kind so its priority rules operate on what actually exists rather
    // than on whatever the first `limit` rows happened to be.
    virtual std::vector<FindRecord> fetchEligibleOfKind(FindKind kind,
                                                        const std::string& now_utc,
                                                        std::size_t limit) = 0;

    // Oldest-first AcceptedUnconfirmed rows whose next_attempt_at is null or <= now_utc, so
    // /get_block confirmation can be retried after a transient lookup failure (contract v1.1;
    // closes the re-drive gap where such rows were unreachable through the interface).
    virtual std::vector<FindRecord> fetchAwaitingConfirmation(const std::string& now_utc,
                                                              std::size_t limit) = 0;

    // Single-record lookup for stats and tests (contract v1.1).
    virtual std::optional<FindRecord> getById(std::int64_t id) = 0;

    // Persist the outcome of one submission attempt (status, reason, attempt bookkeeping,
    // backoff time, http status/response, confirmation timestamp for Acked).
    virtual void recordAttempt(std::int64_t id, const Classification& c,
                               std::optional<int> http_status,
                               const std::string& response_body,
                               const std::optional<std::string>& next_attempt_at,
                               const std::string& now_utc) = 0;

    // ParkedDifficulty -> Pending for all records with m >= current_difficulty.
    // Returns number un-parked.
    virtual std::size_t unparkForDifficulty(std::uint32_t current_difficulty) = 0;

    // ParkedXuniWindow -> Pending for XUNI whose window budget remains; increments
    // xuni_windows_tried; records exceeding max_windows go to Dead. Returns number un-parked.
    virtual std::size_t unparkXuniForWindow(std::int32_t max_windows) = 0;

    // Startup recovery counts, logged at boot. Also resets any in-flight leftovers to Pending.
    struct RecoveryStats {
        std::size_t pending = 0, accepted_unconfirmed = 0, parked_difficulty = 0,
                    parked_xuni = 0, quarantined = 0, acked = 0, dead = 0, invalid = 0;
    };
    virtual RecoveryStats recoverOnStartup() = 0;

    // Difficulty observation log (PLAN v2 §10.7).
    virtual void recordDifficulty(std::uint32_t difficulty, const std::string& at_utc) = 0;
    virtual std::optional<std::uint32_t> lastKnownDifficulty() = 0;

    // Counters for the stats endpoint.
    struct Counts {
        // Union of both consumers: the stats endpoint wants the parked split
        // (parked_difficulty/parked_xuni), the terminal status line wants the per-kind
        // queued split (queued_xen11/queued_xuni = Pending by kind).
        std::size_t pending = 0, parked = 0, parked_difficulty = 0, parked_xuni = 0,
                    quarantined = 0, acked_total = 0, dead_total = 0,
                    accepted_unconfirmed = 0, permanently_invalid = 0,
                    queued_xen11 = 0, queued_xuni = 0;
    };
    virtual Counts counts() = 0;
};

}  // namespace treeminer
