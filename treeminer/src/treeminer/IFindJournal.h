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
        std::size_t pending = 0, parked = 0, quarantined = 0, acked_total = 0, dead_total = 0;
    };
    virtual Counts counts() = 0;
};

}  // namespace treeminer
