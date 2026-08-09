#pragma once
// ResponseClassifier — pure, deterministic classification of XenBlocks /verify responses.
// No I/O, no clock, no globals. The truth table is verified against the reference server
// (repos/xenminer/gpage.py:366-520); see PLAN.md §3.2 and §10 amendments 2-4.
//
// Server facts this encodes:
//   200  "Hash verified successfully and block saved."  (gpage.py:515) — can LIE: the server
//        returns 200 even when its DB insert retries were exhausted (gpage.py:492-494,515),
//        so 200 => AcceptedUnconfirmed + /get_block lookup, never straight Acked.
//   400  "Block already exists, continue"               (gpage.py:510) — UNIQUE key duplicate;
//        a prior attempt landed => AcceptedUnconfirmed(duplicate) + lookup => Acked.
//   401  "Hash does not contain 'm={N}'. ..."           (gpage.py:416) — strictly-< difficulty
//        check; N is the server's CURRENT difficulty => ParkedDifficulty + hint.
//   401  "XUNI Submitted outside of proper time frame." (gpage.py:434) and the legacy
//        "XUNI found outside of time window"            (gpage.py:497) — server-clock gated;
//        ParkedXuniWindow for XUNI, Quarantined (impossible, log loud) for XEN11.
//   401  "Hash verification failed."                    (gpage.py:519) => PermanentlyInvalid.
//   429  => Pending; Retry-After honored by the caller (hint appended to reason).
//   408/425/5xx/timeout/connect error/empty body        => Pending with backoff.
//   anything else                                       => Quarantined (never silent-drop).

#include <cstdint>
#include <optional>
#include <string>

#include "../treeminer/Types.h"

namespace treeminer {

// Sentinel http_status for transport-level failures (connect error, timeout, DNS failure).
// The upstream cpr client also reports status_code 0 on transport errors.
constexpr int kTransportError = 0;

// Classify one /verify response. Fully deterministic.
Classification classify(int http_status, const std::string& body, FindKind kind);

// Overload carrying the raw Retry-After header value (429 handling). When the header parses
// as delay-seconds, "retry_after_s=N" is appended to Classification::reason so the caller can
// honor it when computing next_attempt_at.
Classification classify(int http_status, const std::string& body, FindKind kind,
                        const std::optional<std::string>& retry_after);

// --- helpers exposed for unit tests (all pure) ---

// Extract a top-level string or number field from a JSON object body.
// Returns nullopt when the body is not a JSON object or lacks the key.
std::optional<std::string> extractJsonField(const std::string& body, const std::string& key);

// The server wraps human messages as {"message": ...} (and validation errors as {"error": ...}).
// Structured parse first; the classifier falls back to raw-body substring matching.
std::optional<std::string> extractJsonMessage(const std::string& body);

// Parse the first "m=<digits>" occurrence (the current-difficulty hint embedded in the 401
// difficulty message, gpage.py:416). Returns nullopt when absent or out of uint32 range.
std::optional<std::uint32_t> parseDifficultyHint(const std::string& message);

// Parse a Retry-After header value in delay-seconds form. HTTP-date form returns nullopt.
std::optional<long> parseRetryAfterSeconds(const std::string& header_value);

}  // namespace treeminer
