#pragma once
// CommandEnvelope — HMAC-SHA256 signed-command envelope for platform/MQTT control
// messages (external security review, finding 2: unauthenticated miner takeover).
//
// WHY this exists: the MQTT broker is a shared rendezvous point. Anyone who can
// publish to xenminer/{worker_id}/task or .../control could previously redirect the
// miner's payout address, change its difficulty, or shut it down. This module gives
// every command a verifiable envelope so the miner only obeys a party that holds the
// shared secret (config.txt key `platform_command_secret`).
//
// WHY pure functions with an injected clock: verification must be unit-testable
// without a broker, without paho, and without sleeping. Everything here depends only
// on nlohmann::json and OpenSSL (both already in vcpkg.json).
//
// Envelope wire format — the signer adds an "auth" object to the command JSON:
//
//   {
//     "command": "assign_task", ... command fields ...,
//     "auth": {
//       "worker_id":  "<target worker's machine id>",
//       "command_id": "<issuer-unique id, 1..128 chars [A-Za-z0-9._-]>",
//       "issued_at":  <unix seconds>,
//       "expires_at": <unix seconds, issued_at < expires_at <= issued_at + 900>,
//       "nonce":      "<random hex, 16..128 chars>",
//       "sig":        "<lowercase hex HMAC-SHA256, 64 chars>"
//     }
//   }
//
// The signature covers a canonical string (see signingString):
//
//   "TMv1\n" + worker_id + "\n" + command_id + "\n" + issued_at + "\n"
//            + expires_at + "\n" + nonce + "\n" + canonicalBody(msg)
//
// where canonicalBody is the message with "auth" removed, serialized by
// nlohmann::json::dump() — nlohmann stores object keys sorted, so dump() is
// deterministic and any signer that emits sorted-key compact JSON interoperates.
//
// Replay defense: the nonce of every ACCEPTED command is remembered in a bounded
// NonceCache; a repeat within its validity window is rejected. Nonces are only
// recorded after the signature verifies, so an attacker without the secret cannot
// poison or fill the cache.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace platform {

// --- Bounds (shared by signer, verifier, and the transport layer) ---

// Reject payloads before JSON parsing ever sees them; a broker-connected attacker
// must not be able to make the dispatch thread chew megabytes of nested JSON.
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;

// Hard cap on envelope lifetime. Short lifetimes bound the replay window that the
// NonceCache has to remember, which is what lets the cache stay small and bounded.
constexpr std::int64_t kMaxLifetimeSec = 15 * 60;

// Tolerated clock skew between issuer and miner for the issued-at check.
constexpr std::int64_t kClockSkewSec = 30;

constexpr std::size_t kMinNonceHexLen = 16;   // >= 64 bits of randomness
constexpr std::size_t kMaxNonceHexLen = 128;
constexpr std::size_t kMaxIdLen = 128;        // worker_id / command_id bound

// --- Small validation helpers (also used for command payload field checks) ---

// True iff `s` is entirely [0-9a-fA-F] and min_len <= len <= max_len.
bool isHexString(const std::string& s, std::size_t min_len, std::size_t max_len);

// True iff `s` is entirely [A-Za-z0-9._-] and min_len <= len <= max_len.
// WHY this charset: identifiers travel into logs and MQTT topics; keeping them to a
// conservative set prevents log forging (embedded \n / ANSI) and topic injection.
bool isSafeIdentifier(const std::string& s, std::size_t min_len, std::size_t max_len);

// True iff `s` is printable ASCII (0x20..0x7E) and min_len <= len <= max_len.
bool isPrintableAscii(const std::string& s, std::size_t min_len, std::size_t max_len);

// Lowercase-hex HMAC-SHA256 via OpenSSL (never hand-rolled crypto).
std::string hmacSha256Hex(const std::string& key, const std::string& data);

// Constant-time equality for hex digests (case-insensitive on input; both sides are
// normalized to lowercase before CRYPTO_memcmp). WHY: a naive std::string compare
// leaks the matching prefix length through timing.
bool constantTimeHexEquals(const std::string& a, const std::string& b);

// --- Bounded replay cache ---
//
// Remembers accepted nonces until they expire. FIFO-bounded: when full, the oldest
// entry is evicted. Because envelope lifetime is capped at kMaxLifetimeSec, insertion
// order approximates expiry order, so eviction rarely discards a still-live nonce
// unless capacity is exceeded by genuinely signed traffic (the attacker cannot cause
// that without the secret). Single-threaded by design — the PlatformManager dispatch
// worker is the only caller.
class NonceCache {
public:
    explicit NonceCache(std::size_t capacity);

    // Returns true and records the nonce if it is fresh; false if it was already
    // seen (replay). Expired entries are purged lazily as a side effect.
    bool checkAndInsert(const std::string& nonce, std::int64_t expires_at,
                        std::int64_t now_epoch_s);

    std::size_t size() const { return by_nonce_.size(); }
    std::size_t capacity() const { return capacity_; }

private:
    void purgeExpired(std::int64_t now_epoch_s);

    std::size_t capacity_;
    std::unordered_map<std::string, std::int64_t> by_nonce_;  // nonce -> expires_at
    std::deque<std::string> insertion_order_;
};

// --- Verification ---

enum class VerifyStatus {
    Ok,
    MissingAuth,      // no "auth" object at all (unsigned command)
    MalformedAuth,    // auth present but fields missing/wrong type/out of bounds
    WrongWorker,      // envelope addressed to a different worker id
    IssuedInFuture,   // issued_at ahead of our clock beyond kClockSkewSec
    LifetimeInvalid,  // expires_at <= issued_at, or lifetime > kMaxLifetimeSec
    Expired,          // now past expires_at
    BadSignature,
    ReplayedNonce,
};

const char* verifyStatusName(VerifyStatus status);

// Message with "auth" removed, dumped deterministically (sorted keys, compact).
std::string canonicalBody(nlohmann::json msg);

// The exact byte string the HMAC covers. Exposed so tests and a reference signer
// share one definition with the verifier.
std::string signingString(const std::string& worker_id, const std::string& command_id,
                          std::int64_t issued_at, std::int64_t expires_at,
                          const std::string& nonce, const std::string& body);

// Attach a valid "auth" envelope to `msg` (test/reference-signer helper; the miner
// itself only ever verifies).
nlohmann::json signCommand(nlohmann::json msg, const std::string& secret,
                           const std::string& worker_id, const std::string& command_id,
                           const std::string& nonce, std::int64_t issued_at,
                           std::int64_t expires_at);

// Full envelope check in the order: schema -> addressing -> time window ->
// signature -> replay. The nonce is consumed (inserted) only when everything else
// passed, so failed attempts cannot poison the cache.
VerifyStatus verifyEnvelope(const nlohmann::json& msg, const std::string& secret,
                            const std::string& expected_worker_id,
                            std::int64_t now_epoch_s, NonceCache& nonces);

// --- Policy classification ---
//
// When NO secret is configured (legacy deployments), the miner keeps accepting the
// non-mutating marketplace flow it always accepted — registration acks, lease
// assignment/release, pause/resume — but never state-mutating commands
// (payout address / difficulty / prefix / pattern changes, remote shutdown).
// Fail-closed: unknown commands and unknown control actions count as mutating.
bool isMutatingCommand(const nlohmann::json& msg);

}  // namespace platform
