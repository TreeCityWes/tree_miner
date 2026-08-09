#pragma once
// TreeMiner shared types — the single source of truth for the journal/submitter contract.
// Owned by the integration lead; journal and submitter components include this and must not
// redefine these types. See PLAN.md §3 and the v2 amendments.

#include <cstdint>
#include <optional>
#include <string>

namespace treeminer {

enum class FindKind { XEN11, XUNI };

// Full lifecycle of a find. Terminal states: Acked, Dead, PermanentlyInvalid.
enum class FindStatus {
    Pending,             // durable, awaiting (re)submission
    Submitting,          // claimed by the submitter thread (in-process only; never persisted)
    AcceptedUnconfirmed, // got HTTP 200; awaiting /get_block confirmation
    Acked,               // confirmed stored server-side (200+lookup, or "already exists")
    ParkedDifficulty,    // 401 difficulty-too-low; auto-repending when current difficulty <= m
    ParkedXuniWindow,    // XUNI missed a window; eligible again next :55-:05 (bounded by budget)
    Quarantined,         // unknown 4xx/unknown schema; never auto-unparks, operator visible
    Dead,                // XUNI window budget exhausted
    PermanentlyInvalid,  // malformed / failed local verification; high-severity log
};

// Immutable capture of a find at discovery time, built ONLY from the parameters the GPU batch
// actually used (fixes Woody's stale-difficulty silent drop, upstream main.cpp:371-381).
// Never recompute hash_to_verify after construction.
struct FoundPayload {
    std::string key;            // 64-hex Argon2 password; server-side dedupe key
    std::string hash_to_verify; // complete PHC string incl. m= from the ORIGINAL batch
    std::string account;        // 0x-prefixed reward address (also the salt source)
    FindKind kind;
    std::uint32_t memory_cost;  // m baked into hash_to_verify
    std::string worker;
    std::uint64_t attempts;
    double hashes_per_second;
    std::string found_at_utc;   // ISO-8601; local bookkeeping only (server never sees it)
};

// A journaled find: payload + durable lifecycle state.
struct FindRecord {
    std::int64_t id = -1;
    FoundPayload payload;
    FindStatus status = FindStatus::Pending;
    std::string status_reason;
    std::int32_t attempt_count = 0;
    std::optional<std::string> next_attempt_at;  // ISO-8601 UTC; persisted so restarts keep backoff
    std::optional<std::string> last_attempt_at;
    std::optional<int> last_http_status;
    std::string last_response;
    std::optional<std::string> confirmed_at;
    std::int32_t xuni_windows_tried = 0;
};

// Outcome of classifying one server response. Pure data; produced by ResponseClassifier.
struct Classification {
    FindStatus next_status;
    // When the 401 difficulty message embeds the current difficulty (m={N}), it is surfaced
    // here so the difficulty cache updates without waiting for the poller.
    std::optional<std::uint32_t> server_difficulty_hint;
    bool needs_lookup_confirmation = false;  // true for 200 and duplicate responses
    std::string reason;                      // human-readable, stored in status_reason
};

const char* to_string(FindStatus s);
const char* to_string(FindKind k);

}  // namespace treeminer
