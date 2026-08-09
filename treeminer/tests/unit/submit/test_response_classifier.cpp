// ResponseClassifier unit tests — one test per truth-table row, with the REAL server
// strings from repos/xenminer/gpage.py (line references in comments).

#include <string>

#include "submit/ResponseClassifier.h"
#include "test_framework.h"

using treeminer::Classification;
using treeminer::FindKind;
using treeminer::FindStatus;
using treeminer::classify;
using treeminer::kTransportError;

int main() {
    // --- Row: 200 -> AcceptedUnconfirmed, needs_lookup_confirmation (gpage.py:515,
    // lying-200 risk at gpage.py:492-494) ---
    TEST_CASE("200 success is only AcceptedUnconfirmed");
    {
        auto c = classify(200, R"({"message": "Hash verified successfully and block saved."})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::AcceptedUnconfirmed);
        CHECK(c.needs_lookup_confirmation);
        CHECK(!c.server_difficulty_hint.has_value());
    }
    {
        auto c = classify(200, R"({"message": "Hash verified successfully and block saved."})",
                          FindKind::XUNI);
        CHECK(c.next_status == FindStatus::AcceptedUnconfirmed);
        CHECK(c.needs_lookup_confirmation);
    }

    // --- Row: 400 "already exists" -> AcceptedUnconfirmed duplicate (gpage.py:510) ---
    TEST_CASE("400 duplicate key acks via lookup");
    {
        auto c = classify(400, R"({"message": "Block already exists, continue"})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::AcceptedUnconfirmed);
        CHECK(c.needs_lookup_confirmation);
        CHECK(c.reason.find("duplicate") != std::string::npos);
    }
    {
        // Substring fallback: non-JSON body containing the marker still classifies.
        auto c = classify(400, "Block already exists, continue", FindKind::XUNI);
        CHECK(c.next_status == FindStatus::AcceptedUnconfirmed);
        CHECK(c.needs_lookup_confirmation);
    }

    // --- Row: 401 difficulty message -> ParkedDifficulty + m={N} hint (gpage.py:416) ---
    TEST_CASE("401 difficulty message parks and surfaces the hint");
    {
        auto c = classify(401,
                          R"({"message": "Hash does not contain 'm=104000'. Your memory_cost setting in your miner will be autoadjusted."})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::ParkedDifficulty);
        CHECK(c.server_difficulty_hint.has_value());
        CHECK_EQ(c.server_difficulty_hint.value_or(0), 104000u);
        CHECK(!c.needs_lookup_confirmation);
    }
    {
        // Substring fallback (no JSON wrapper).
        auto c = classify(401, "Hash does not contain 'm=99000'. Your memory_cost setting "
                               "in your miner will be autoadjusted.",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::ParkedDifficulty);
        CHECK_EQ(c.server_difficulty_hint.value_or(0), 99000u);
    }

    // --- Row: 401 XUNI window, CURRENT server string (gpage.py:434) ---
    TEST_CASE("401 XUNI window (current string) parks XUNI");
    {
        auto c = classify(401, R"({"message": "XUNI Submitted outside of proper time frame."})",
                          FindKind::XUNI);
        CHECK(c.next_status == FindStatus::ParkedXuniWindow);
    }

    // --- Row: 401 XUNI window, LEGACY variant (gpage.py:497) ---
    TEST_CASE("401 XUNI window (legacy string) parks XUNI");
    {
        auto c = classify(401, R"({"message": "XUNI found outside of time window"})",
                          FindKind::XUNI);
        CHECK(c.next_status == FindStatus::ParkedXuniWindow);
    }

    // --- Row: XUNI-window rejection for a XEN11 record is impossible (docs/05 §2) ->
    // Quarantined, loud ---
    TEST_CASE("401 XUNI window for XEN11 is quarantined loudly");
    {
        auto c = classify(401, R"({"message": "XUNI Submitted outside of proper time frame."})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
        CHECK(c.reason.find("IMPOSSIBLE") != std::string::npos);
    }
    {
        auto c = classify(401, R"({"message": "XUNI found outside of time window"})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
        CHECK(c.reason.find("IMPOSSIBLE") != std::string::npos);
    }

    // --- Row: 401 "Hash verification failed." -> PermanentlyInvalid (gpage.py:519) ---
    TEST_CASE("401 verification failure is permanently invalid");
    {
        auto c = classify(401, R"({"message": "Hash verification failed."})", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::PermanentlyInvalid);
    }
    {
        auto c = classify(401, R"({"message": "Hash verification failed."})", FindKind::XUNI);
        CHECK(c.next_status == FindStatus::PermanentlyInvalid);
    }

    // --- Row: 429 -> Pending, Retry-After honored via reason hint ---
    TEST_CASE("429 stays pending and honors Retry-After");
    {
        auto c = classify(429, R"({"message": "slow down"})", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(429, R"({"message": "slow down"})", FindKind::XEN11,
                          std::optional<std::string>("30"));
        CHECK(c.next_status == FindStatus::Pending);
        CHECK(c.reason.find("retry_after_s=30") != std::string::npos);
    }
    {
        // HTTP-date Retry-After is unsupported: still Pending, no hint.
        auto c = classify(429, R"({"message": "slow down"})", FindKind::XEN11,
                          std::optional<std::string>("Wed, 21 Oct 2015 07:28:00 GMT"));
        CHECK(c.next_status == FindStatus::Pending);
        CHECK(c.reason.find("retry_after_s=") == std::string::npos);
    }

    // --- Row: 408/425/5xx/transport error/empty body -> Pending with backoff ---
    TEST_CASE("transport-class failures stay pending");
    {
        auto c = classify(kTransportError, "", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(408, "Request Timeout", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(425, "Too Early", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(500, "<html>Internal Server Error</html>", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(502, "Bad Gateway", FindKind::XUNI);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(503, R"({"message": "Service Unavailable"})", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        // Empty body, even on 200: never conclusive (mock server "empty-body" fault).
        auto c = classify(200, "", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }
    {
        auto c = classify(200, "   \n", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Pending);
    }

    // --- Row: any other 4xx / unrecognized body -> Quarantined ---
    TEST_CASE("unknown responses quarantine, never drop");
    {
        // Real validation 400s from gpage.py:391,395,399 ({"error": ...} schema).
        auto c = classify(400, R"({"error": "Invalid key format"})", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        auto c = classify(400, R"({"error": "Invalid salt format"})", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        auto c = classify(400, R"({"error": "Missing hash_to_verify, key, or account"})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        // gpage.py:439 targets message: a 401 with no m= digits and no XUNI marker.
        auto c = classify(401,
                          R"({"message": "Hash does not contain any of the valid targets ['XEN11'] in the last 87 characters. Adjust target_substr in your miner."})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        auto c = classify(403, "Forbidden", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        auto c = classify(404, "Not Found", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        auto c = classify(301, "Moved Permanently", FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }

    // --- Structured parse before substring fallback ---
    TEST_CASE("JSON message field wins over incidental body text");
    {
        // The marker only appears inside a different JSON field; the "message" field is
        // unrecognized -> Quarantined (no naive whole-body substring hit on "message").
        auto c = classify(401, R"({"debug": "Hash verification failed.", "message": "totally new response"})",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::Quarantined);
    }
    {
        // Escaped quotes inside the message decode correctly.
        auto c = classify(401,
                          "{\"message\": \"Hash does not contain \\u0027m=123456\\u0027.\"}",
                          FindKind::XEN11);
        CHECK(c.next_status == FindStatus::ParkedDifficulty);
        CHECK_EQ(c.server_difficulty_hint.value_or(0), 123456u);
    }

    // --- helper coverage ---
    TEST_CASE("extractJsonField and parseDifficultyHint helpers");
    {
        auto v = treeminer::extractJsonField(R"({"difficulty": "104000"})", "difficulty");
        CHECK(v.has_value());
        CHECK_STREQ(v.value_or(""), "104000");
    }
    {
        auto v = treeminer::extractJsonField(R"({"a": 1, "difficulty": 98000})", "difficulty");
        CHECK(v.has_value());
        CHECK_STREQ(v.value_or(""), "98000");
    }
    {
        CHECK(!treeminer::extractJsonField("not json", "difficulty").has_value());
        CHECK(!treeminer::extractJsonField("{}", "difficulty").has_value());
    }
    {
        CHECK_EQ(treeminer::parseDifficultyHint("m=1234 tail").value_or(0), 1234u);
        CHECK(!treeminer::parseDifficultyHint("no hint here").has_value());
        CHECK(!treeminer::parseDifficultyHint("m=99999999999999").has_value());  // > uint32
        CHECK_EQ(treeminer::parseDifficultyHint("memory m=x then m=42").value_or(0), 42u);
    }
    {
        CHECK_EQ(treeminer::parseRetryAfterSeconds("120").value_or(-1), 120L);
        CHECK_EQ(treeminer::parseRetryAfterSeconds(" 5 ").value_or(-1), 5L);
        CHECK(!treeminer::parseRetryAfterSeconds("Wed, 21 Oct 2015 07:28:00 GMT").has_value());
    }

    // --- determinism spot check ---
    TEST_CASE("classification is deterministic");
    {
        const std::string body = R"({"message": "Hash does not contain 'm=104000'."})";
        auto a = classify(401, body, FindKind::XEN11);
        auto b = classify(401, body, FindKind::XEN11);
        CHECK(a.next_status == b.next_status);
        CHECK_EQ(a.server_difficulty_hint.value_or(0), b.server_difficulty_hint.value_or(0));
        CHECK_STREQ(a.reason, b.reason);
    }

    return testfw::summary("response_classifier");
}
