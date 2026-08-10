#pragma once
// FindJournal.h — SQLite-backed durable find journal (implements treeminer::IFindJournal).
// See PLAN.md §3.1 + §10 and SOL-PLAN.md §4 for the schema and durability contract.
//
// Durability: the database is opened with journal_mode=WAL and synchronous=FULL, so every
// committed transaction is fsync'd to the WAL before the call returns ("journal-first"
// invariant: append() returns only after the find is crash-safe on disk). A WAL checkpoint
// is NOT required for durability.
//
// Concurrency: every public method takes an internal mutex, so one instance may be shared
// by the mining and submission threads. Use exactly ONE FindJournal instance per process,
// with a stable per-process database path; never place the database on NFS/shared storage
// (PLAN v2 §10.9, SOL-PLAN §4).
//
// Time: all timestamps are caller-provided ISO-8601 UTC strings. The journal never reads
// a clock itself, which keeps every code path deterministic under test.
//
// Errors: any SQLite failure throws treeminer::JournalError carrying the SQLite error
// text. No error is swallowed; no resource leaks on any path (RAII wrappers own both the
// sqlite3 connection and every prepared statement).

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

#include "treeminer/IFindJournal.h"

struct sqlite3;  // opaque; <sqlite3.h> is confined to FindJournal.cpp

namespace treeminer {

// Thrown on any SQLite or contract failure; what() carries the SQLite error text.
class JournalError : public std::runtime_error {
public:
    explicit JournalError(const std::string& message) : std::runtime_error(message) {}
};

class FindJournal final : public IFindJournal {
public:
    // Opens (creating if absent) the database at dbPath, applies the durability pragmas
    // (WAL, synchronous=FULL, busy_timeout=5000, foreign_keys=ON) and creates or
    // validates the schema (schema_version 1). Throws JournalError on any failure,
    // including a database whose schema_version is newer than this build supports.
    explicit FindJournal(const std::string& dbPath);
    ~FindJournal() override;

    FindJournal(const FindJournal&) = delete;
    FindJournal& operator=(const FindJournal&) = delete;

    // Durable on return (single fsync'd transaction). Duplicate key returns the existing
    // row's id. Payloads that violate server limits (hash_to_verify longer than 150
    // characters, empty account) are still stored — never throw away a find — but with
    // status PermanentlyInvalid and a status_reason describing the violation.
    std::int64_t append(const FoundPayload& payload) override;

    std::vector<FindRecord> fetchEligible(const std::string& now_utc,
                                          std::size_t limit) override;

    // Same eligibility, ordering and backoff semantics as fetchEligible, restricted to one
    // kind so neither kind can be starved out of a mixed LIMIT slice by the other.
    std::vector<FindRecord> fetchEligibleOfKind(FindKind kind, const std::string& now_utc,
                                                std::size_t limit) override;

    // Contract v1.1: oldest-first AcceptedUnconfirmed rows whose next_attempt_at is NULL
    // or <= now_utc, so /get_block confirmation can be re-driven after a transient lookup
    // failure. Same eligibility/backoff semantics and row hydration as fetchEligible.
    std::vector<FindRecord> fetchAwaitingConfirmation(const std::string& now_utc,
                                                      std::size_t limit) override;

    // Contract v1.1: single-record lookup; std::nullopt when no such id.
    std::optional<FindRecord> getById(std::int64_t id) override;

    // Persists one attempt outcome. attempt_count is incremented, last_attempt_at is set
    // to now_utc, and when the new status is Acked, confirmed_at is set to now_utc (first
    // confirmation wins; a repeated Ack does not move it). Classification.next_status must
    // not be Submitting — that state is in-process only and never persisted (throws).
    void recordAttempt(std::int64_t id, const Classification& c,
                       std::optional<int> http_status,
                       const std::string& response_body,
                       const std::optional<std::string>& next_attempt_at,
                       const std::string& now_utc) override;

    // ParkedDifficulty -> Pending for rows with m >= current_difficulty (boundary
    // inclusive: the server check is strictly submitted_m < difficulty). Clears
    // next_attempt_at so the drain picks them up immediately.
    std::size_t unparkForDifficulty(std::uint32_t current_difficulty) override;

    // ParkedXuniWindow rows with remaining budget (xuni_windows_tried < max_windows) go
    // to Pending with the counter incremented; the rest go to Dead with reason
    // "xuni window budget exhausted". Returns the number un-parked.
    std::size_t unparkXuniForWindow(std::int32_t max_windows) override;

    // Resets any persisted 'Submitting' leftovers to Pending (there should be none — the
    // state is never persisted; this is defense in depth) and returns per-state counts.
    // Persisted next_attempt_at backoffs are deliberately kept: restarts must not reset
    // backoff (SOL-PLAN §6).
    RecoveryStats recoverOnStartup() override;

    void recordDifficulty(std::uint32_t difficulty, const std::string& at_utc) override;
    std::optional<std::uint32_t> lastKnownDifficulty() override;

    // Strict per-state mapping: pending=Pending, parked=ParkedDifficulty+ParkedXuniWindow,
    // quarantined=Quarantined, acked_total=Acked, dead_total=Dead, plus the v1.1 slots
    // accepted_unconfirmed=AcceptedUnconfirmed and permanently_invalid=PermanentlyInvalid.
    Counts counts() override;

private:
    void applyPragmas();
    void ensureSchema();
    void execOrThrow(const char* sql, const char* context);
    // Shared query body for fetchEligible / fetchAwaitingConfirmation. Caller holds mutex_;
    // statusLiteral must be a trusted compile-time status string, never user input.
    // kindLiteral is nullptr for "any kind", otherwise a trusted compile-time kind string.
    std::vector<FindRecord> fetchByStatusLocked(const char* statusLiteral,
                                                const std::string& now_utc,
                                                std::size_t limit,
                                                const char* kindLiteral = nullptr);

    sqlite3* db_ = nullptr;
    std::mutex mutex_;  // guards db_ across all public methods
};

}  // namespace treeminer
