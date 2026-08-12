// CommandEnvelope unit tests — signature/expiry/nonce/replay verification logic
// (security review finding 2). Pure logic: no paho, no broker, injected clock.
//
// CMake registration for the integrator (root CMakeLists is owned elsewhere).
// Suggested tests/unit/platform/CMakeLists.txt:
//
//   # Unit tests for platform command authentication. Needs nlohmann-json headers
//   # and OpenSSL::Crypto (both already vcpkg dependencies of the main target).
//   add_executable(test_command_envelope
//       test_command_envelope.cpp
//       ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/platform/CommandEnvelope.cpp)
//   target_include_directories(test_command_envelope PRIVATE
//       ${CMAKE_CURRENT_SOURCE_DIR}
//       ${CMAKE_CURRENT_SOURCE_DIR}/../submit          # test_framework.h
//       ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
//   find_package(OpenSSL REQUIRED)
//   find_package(nlohmann_json CONFIG REQUIRED)
//   target_link_libraries(test_command_envelope PRIVATE
//       OpenSSL::Crypto nlohmann_json::nlohmann_json)
//   add_test(NAME platform.command_envelope COMMAND test_command_envelope)
//
// Manual build (as verified):
//   g++ -std=c++17 -I src -I tests/unit/submit \
//       -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/platform/test_command_envelope.cpp src/platform/CommandEnvelope.cpp \
//       -L build/vcpkg_installed/x64-linux/lib -lcrypto -lpthread -ldl -o test_command_envelope

#include <cstdint>

#include "platform/CommandEnvelope.h"
#include "test_framework.h"

using platform::NonceCache;
using platform::VerifyStatus;

namespace {

const std::string kSecret = "correct horse battery staple";
const std::string kWorker = "rig-01";
const std::string kNonce = "0123456789abcdef0011223344556677";

nlohmann::json baseCommand() {
    return nlohmann::json{{"command", "assign_task"},
                          {"lease_id", "L-1"},
                          {"consumer_id", "C-1"},
                          {"duration_sec", 3600}};
}

// Signs baseCommand() with sane defaults relative to `now`.
nlohmann::json signedCommand(std::int64_t now, const std::string& nonce = kNonce,
                             const std::string& worker = kWorker,
                             const std::string& secret = kSecret) {
    return platform::signCommand(baseCommand(), secret, worker, "cmd-1", nonce,
                                 /*issued_at=*/now, /*expires_at=*/now + 60);
}

}  // namespace

int main() {
    const std::int64_t now = 1'700'000'000;

    TEST_CASE("hmacSha256Hex matches RFC 4231 / known-answer vector");
    {
        // Classic known-answer test: HMAC-SHA256("key", "The quick brown fox...")
        CHECK_STREQ(platform::hmacSha256Hex(
                        "key", "The quick brown fox jumps over the lazy dog"),
                    "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    }

    TEST_CASE("valid signed command verifies Ok");
    {
        NonceCache cache(16);
        CHECK(platform::verifyEnvelope(signedCommand(now), kSecret, kWorker, now, cache) ==
              VerifyStatus::Ok);
    }

    TEST_CASE("unsigned command -> MissingAuth");
    {
        NonceCache cache(16);
        CHECK(platform::verifyEnvelope(baseCommand(), kSecret, kWorker, now, cache) ==
              VerifyStatus::MissingAuth);
    }

    TEST_CASE("malformed auth fields -> MalformedAuth");
    {
        NonceCache cache(16);
        auto msg = signedCommand(now);
        auto broken = msg;
        broken["auth"].erase("nonce");
        CHECK(platform::verifyEnvelope(broken, kSecret, kWorker, now, cache) ==
              VerifyStatus::MalformedAuth);

        broken = msg;
        broken["auth"]["issued_at"] = "not-a-number";
        CHECK(platform::verifyEnvelope(broken, kSecret, kWorker, now, cache) ==
              VerifyStatus::MalformedAuth);

        broken = msg;
        broken["auth"]["nonce"] = "zzzz";  // non-hex and too short
        CHECK(platform::verifyEnvelope(broken, kSecret, kWorker, now, cache) ==
              VerifyStatus::MalformedAuth);

        broken = msg;
        broken["auth"]["sig"] = "abcd";  // wrong digest length
        CHECK(platform::verifyEnvelope(broken, kSecret, kWorker, now, cache) ==
              VerifyStatus::MalformedAuth);
    }

    TEST_CASE("envelope for another worker -> WrongWorker");
    {
        NonceCache cache(16);
        auto msg = signedCommand(now, kNonce, /*worker=*/"rig-02");
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::WrongWorker);
    }

    TEST_CASE("time-window checks: future / lifetime / expired");
    {
        NonceCache cache(16);
        // issued too far in the future (beyond skew)
        auto msg = platform::signCommand(baseCommand(), kSecret, kWorker, "cmd-f", kNonce,
                                         now + platform::kClockSkewSec + 5,
                                         now + platform::kClockSkewSec + 65);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::IssuedInFuture);

        // within skew tolerance is accepted
        msg = platform::signCommand(baseCommand(), kSecret, kWorker, "cmd-s", kNonce,
                                    now + platform::kClockSkewSec - 1,
                                    now + platform::kClockSkewSec + 59);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::Ok);

        // lifetime longer than the hard cap
        msg = platform::signCommand(baseCommand(), kSecret, kWorker, "cmd-l",
                                    "aaaabbbbccccdddd", now,
                                    now + platform::kMaxLifetimeSec + 1);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::LifetimeInvalid);

        // expires_at <= issued_at
        msg = platform::signCommand(baseCommand(), kSecret, kWorker, "cmd-z",
                                    "aaaabbbbccccdddd", now, now);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::LifetimeInvalid);

        // already expired
        msg = platform::signCommand(baseCommand(), kSecret, kWorker, "cmd-e",
                                    "aaaabbbbccccdddd", now - 120, now - 60);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::Expired);
    }

    TEST_CASE("tampering breaks the signature");
    {
        NonceCache cache(16);
        // body tampered after signing (the takeover scenario: swap the payload)
        auto msg = signedCommand(now);
        msg["lease_id"] = "L-evil";
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::BadSignature);

        // envelope field tampered (extend expiry)
        msg = signedCommand(now);
        msg["auth"]["expires_at"] = now + 120;
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::BadSignature);

        // signed with the wrong secret
        msg = signedCommand(now, kNonce, kWorker, "wrong-secret");
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::BadSignature);
    }

    TEST_CASE("replay of an accepted command is rejected");
    {
        NonceCache cache(16);
        auto msg = signedCommand(now);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::Ok);
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::ReplayedNonce);
        // still rejected later within the validity window
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now + 30, cache) ==
              VerifyStatus::ReplayedNonce);
    }

    TEST_CASE("failed signature does not consume the nonce");
    {
        NonceCache cache(16);
        auto forged = signedCommand(now);
        forged["lease_id"] = "L-evil";  // invalidates sig, same nonce
        CHECK(platform::verifyEnvelope(forged, kSecret, kWorker, now, cache) ==
              VerifyStatus::BadSignature);
        // The genuine command with that nonce must still be accepted: attackers
        // without the secret cannot poison the replay cache.
        CHECK(platform::verifyEnvelope(signedCommand(now), kSecret, kWorker, now, cache) ==
              VerifyStatus::Ok);
    }

    TEST_CASE("nonces expire out of the cache; expired replay is caught by Expired");
    {
        NonceCache cache(16);
        auto msg = signedCommand(now);  // expires_at = now + 60
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now, cache) ==
              VerifyStatus::Ok);
        // After expiry the cache may forget the nonce — the time-window check
        // (which runs before the replay check) still rejects the stale message.
        CHECK(platform::verifyEnvelope(msg, kSecret, kWorker, now + 61, cache) ==
              VerifyStatus::Expired);
    }

    TEST_CASE("NonceCache is bounded (FIFO eviction at capacity)");
    {
        NonceCache cache(4);
        for (int i = 0; i < 10; ++i) {
            const std::string nonce = "00000000000000a" + std::to_string(i);
            CHECK(cache.checkAndInsert(nonce, now + 600, now));
        }
        CHECK(cache.size() <= 4);
        // newest entries are retained
        CHECK(!cache.checkAndInsert("00000000000000a9", now + 600, now));
        // oldest were evicted (re-insert succeeds — bounded-memory tradeoff)
        CHECK(cache.checkAndInsert("00000000000000a0", now + 600, now));
    }

    TEST_CASE("NonceCache purges expired entries lazily");
    {
        NonceCache cache(16);
        CHECK(cache.checkAndInsert("aaaaaaaaaaaaaaaa", now + 10, now));
        CHECK(!cache.checkAndInsert("aaaaaaaaaaaaaaaa", now + 10, now));
        // once expired, the slot is reclaimed and the nonce reusable
        CHECK(cache.checkAndInsert("aaaaaaaaaaaaaaaa", now + 100, now + 11));
        CHECK_EQ(cache.size(), static_cast<std::size_t>(1));
    }

    TEST_CASE("isMutatingCommand policy classification");
    {
        using platform::isMutatingCommand;
        // legacy non-mutating marketplace flow stays allowed without a secret
        CHECK(!isMutatingCommand({{"command", "register_ack"}}));
        CHECK(!isMutatingCommand({{"command", "assign_task"}}));
        CHECK(!isMutatingCommand({{"command", "release"}}));
        CHECK(!isMutatingCommand({{"action", "pause"}}));
        CHECK(!isMutatingCommand({{"action", "resume"}}));
        // mutating: always requires a signature
        CHECK(isMutatingCommand({{"action", "set_config"},
                                 {"config", {{"address", "0xdead"}}}}));
        CHECK(isMutatingCommand({{"action", "shutdown"}}));
        // fail closed on anything unknown or malformed
        CHECK(isMutatingCommand({{"command", "mystery"}}));
        CHECK(isMutatingCommand({{"action", "mystery"}}));
        CHECK(isMutatingCommand(nlohmann::json::array()));
        CHECK(isMutatingCommand(nlohmann::json::object()));
    }

    TEST_CASE("field validators");
    {
        using platform::isHexString;
        using platform::isPrintableAscii;
        using platform::isSafeIdentifier;
        CHECK(isHexString("0123456789abcdefABCDEF", 1, 64));
        CHECK(!isHexString("0x1234", 1, 64));       // '0x' prefix is not hex
        CHECK(!isHexString("abcd", 5, 64));         // too short
        CHECK(!isHexString("abcd", 1, 3));          // too long
        CHECK(isSafeIdentifier("lease_1.a-B", 1, 64));
        CHECK(!isSafeIdentifier("evil\nlog", 1, 64));
        CHECK(!isSafeIdentifier("", 1, 64));
        CHECK(isPrintableAscii("XEN11", 1, 16));
        CHECK(!isPrintableAscii("bad\x01", 1, 16));
    }

    TEST_CASE("canonical body is stable under key insertion order");
    {
        nlohmann::json a{{"b", 1}, {"a", 2}};
        nlohmann::json b;
        b["a"] = 2;
        b["b"] = 1;
        CHECK_STREQ(platform::canonicalBody(a), platform::canonicalBody(b));
    }

    return testfw::summary("command_envelope");
}
