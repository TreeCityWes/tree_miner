// CommandEnvelope implementation — see header for the wire format and threat model.

#include "platform/CommandEnvelope.h"

#include <algorithm>
#include <cctype>

#include <openssl/crypto.h>  // CRYPTO_memcmp
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace platform {

namespace {

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Fetch a signed-integer field, rejecting floats/strings/bools. nlohmann's value()
// would silently coerce or throw; the envelope wants a hard, typed schema.
bool getInt64Field(const nlohmann::json& obj, const char* key, std::int64_t& out) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return false;
    out = it->get<std::int64_t>();
    return true;
}

bool getStringField(const nlohmann::json& obj, const char* key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

}  // namespace

bool isHexString(const std::string& s, std::size_t min_len, std::size_t max_len) {
    if (s.size() < min_len || s.size() > max_len) return false;
    return std::all_of(s.begin(), s.end(), isHexDigit);
}

bool isSafeIdentifier(const std::string& s, std::size_t min_len, std::size_t max_len) {
    if (s.size() < min_len || s.size() > max_len) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

bool isPrintableAscii(const std::string& s, std::size_t min_len, std::size_t max_len) {
    if (s.size() < min_len || s.size() > max_len) return false;
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return c >= 0x20 && c <= 0x7E; });
}

std::string hmacSha256Hex(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    // One-shot OpenSSL HMAC (never hand-rolled). Key may be empty in tests; OpenSSL
    // handles zero-length keys per RFC 2104.
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_len);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0F]);
    }
    return out;
}

bool constantTimeHexEquals(const std::string& a, const std::string& b) {
    // Length is not secret (digest length is fixed and public); only content is.
    if (a.size() != b.size()) return false;
    const std::string la = toLowerAscii(a);
    const std::string lb = toLowerAscii(b);
    return CRYPTO_memcmp(la.data(), lb.data(), la.size()) == 0;
}

// --- NonceCache ---

NonceCache::NonceCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void NonceCache::purgeExpired(std::int64_t now_epoch_s) {
    // Insertion order approximates expiry order (lifetime is capped), so sweeping
    // from the front is enough and stays O(expired) amortized.
    while (!insertion_order_.empty()) {
        auto it = by_nonce_.find(insertion_order_.front());
        if (it != by_nonce_.end() && it->second > now_epoch_s) break;
        if (it != by_nonce_.end()) by_nonce_.erase(it);
        insertion_order_.pop_front();
    }
}

bool NonceCache::checkAndInsert(const std::string& nonce, std::int64_t expires_at,
                                std::int64_t now_epoch_s) {
    purgeExpired(now_epoch_s);

    auto it = by_nonce_.find(nonce);
    if (it != by_nonce_.end()) {
        return false;  // replay
    }

    // FIFO eviction keeps memory bounded no matter how much *validly signed*
    // traffic arrives; unsigned floods never reach this point.
    while (by_nonce_.size() >= capacity_ && !insertion_order_.empty()) {
        by_nonce_.erase(insertion_order_.front());
        insertion_order_.pop_front();
    }

    by_nonce_.emplace(nonce, expires_at);
    insertion_order_.push_back(nonce);
    return true;
}

// --- Verification ---

const char* verifyStatusName(VerifyStatus status) {
    switch (status) {
        case VerifyStatus::Ok:              return "ok";
        case VerifyStatus::MissingAuth:     return "missing auth envelope";
        case VerifyStatus::MalformedAuth:   return "malformed auth envelope";
        case VerifyStatus::WrongWorker:     return "wrong worker id";
        case VerifyStatus::IssuedInFuture:  return "issued in the future";
        case VerifyStatus::LifetimeInvalid: return "invalid lifetime";
        case VerifyStatus::Expired:         return "expired";
        case VerifyStatus::BadSignature:    return "bad signature";
        case VerifyStatus::ReplayedNonce:   return "replayed nonce";
    }
    return "unknown";
}

std::string canonicalBody(nlohmann::json msg) {
    msg.erase("auth");
    // nlohmann::json keeps object keys sorted, so dump() is deterministic: the same
    // logical message always serializes to the same bytes on signer and verifier.
    return msg.dump();
}

std::string signingString(const std::string& worker_id, const std::string& command_id,
                          std::int64_t issued_at, std::int64_t expires_at,
                          const std::string& nonce, const std::string& body) {
    // "TMv1" domain-separates this MAC from any other HMAC use of the same secret
    // and gives us a version handle if the format ever has to change. Every field is
    // newline-delimited; none of the fields may contain '\n' (identifier/hex/number
    // charsets enforce that), so the encoding is unambiguous.
    std::string out;
    out.reserve(64 + worker_id.size() + command_id.size() + nonce.size() + body.size());
    out += "TMv1\n";
    out += worker_id;
    out += '\n';
    out += command_id;
    out += '\n';
    out += std::to_string(issued_at);
    out += '\n';
    out += std::to_string(expires_at);
    out += '\n';
    out += nonce;
    out += '\n';
    out += body;
    return out;
}

nlohmann::json signCommand(nlohmann::json msg, const std::string& secret,
                           const std::string& worker_id, const std::string& command_id,
                           const std::string& nonce, std::int64_t issued_at,
                           std::int64_t expires_at) {
    const std::string body = canonicalBody(msg);
    const std::string sig = hmacSha256Hex(
        secret, signingString(worker_id, command_id, issued_at, expires_at, nonce, body));
    msg["auth"] = {
        {"worker_id", worker_id},   {"command_id", command_id},
        {"issued_at", issued_at},   {"expires_at", expires_at},
        {"nonce", nonce},           {"sig", sig},
    };
    return msg;
}

VerifyStatus verifyEnvelope(const nlohmann::json& msg, const std::string& secret,
                            const std::string& expected_worker_id,
                            std::int64_t now_epoch_s, NonceCache& nonces) {
    if (!msg.is_object()) return VerifyStatus::MissingAuth;

    auto auth_it = msg.find("auth");
    if (auth_it == msg.end()) return VerifyStatus::MissingAuth;
    if (!auth_it->is_object()) return VerifyStatus::MalformedAuth;
    const nlohmann::json& auth = *auth_it;

    // 1. Schema: every field typed and bounded before anything else looks at it.
    std::string worker_id, command_id, nonce, sig;
    std::int64_t issued_at = 0, expires_at = 0;
    if (!getStringField(auth, "worker_id", worker_id) ||
        !getStringField(auth, "command_id", command_id) ||
        !getStringField(auth, "nonce", nonce) ||
        !getStringField(auth, "sig", sig) ||
        !getInt64Field(auth, "issued_at", issued_at) ||
        !getInt64Field(auth, "expires_at", expires_at)) {
        return VerifyStatus::MalformedAuth;
    }
    if (!isSafeIdentifier(worker_id, 1, kMaxIdLen) ||
        !isSafeIdentifier(command_id, 1, kMaxIdLen) ||
        !isHexString(nonce, kMinNonceHexLen, kMaxNonceHexLen) ||
        !isHexString(sig, 64, 64)) {  // HMAC-SHA256 is exactly 32 bytes / 64 hex
        return VerifyStatus::MalformedAuth;
    }

    // 2. Addressing: an envelope signed for a different rig must not work here
    //    (prevents cross-worker replay on a shared broker).
    if (worker_id != expected_worker_id) return VerifyStatus::WrongWorker;

    // 3. Time window. issued_at gets skew tolerance; expiry is strict because the
    //    signer controls it and can add margin.
    if (issued_at > now_epoch_s + kClockSkewSec) return VerifyStatus::IssuedInFuture;
    if (expires_at <= issued_at || expires_at - issued_at > kMaxLifetimeSec) {
        return VerifyStatus::LifetimeInvalid;
    }
    if (now_epoch_s > expires_at) return VerifyStatus::Expired;

    // 4. Signature — over the canonical body plus every envelope field checked above.
    const std::string expected_sig = hmacSha256Hex(
        secret, signingString(worker_id, command_id, issued_at, expires_at, nonce,
                              canonicalBody(msg)));
    if (!constantTimeHexEquals(sig, expected_sig)) return VerifyStatus::BadSignature;

    // 5. Replay — last, so only authentically signed commands consume cache slots.
    if (!nonces.checkAndInsert(nonce, expires_at, now_epoch_s)) {
        return VerifyStatus::ReplayedNonce;
    }
    return VerifyStatus::Ok;
}

bool isMutatingCommand(const nlohmann::json& msg) {
    if (!msg.is_object()) return true;  // fail closed

    const std::string command =
        msg.contains("command") && msg["command"].is_string()
            ? msg["command"].get<std::string>() : "";

    // The historical non-mutating marketplace flow: these only steer which lease the
    // rig serves or pause/resume availability — they never change payout identity,
    // difficulty, key prefix, or block pattern, and cannot kill the process.
    if (command == "register_ack" || command == "assign_task" || command == "release") {
        return false;
    }

    if (command.empty()) {  // control-topic message, keyed by "action"
        const std::string action =
            msg.contains("action") && msg["action"].is_string()
                ? msg["action"].get<std::string>() : "";
        if (action == "pause" || action == "resume") return false;
        // "set_config" (payout address / difficulty / prefix / pattern) and
        // "shutdown" (kills mining outright) always require a signature.
        return true;
    }

    return true;  // unknown command: fail closed
}

}  // namespace platform
