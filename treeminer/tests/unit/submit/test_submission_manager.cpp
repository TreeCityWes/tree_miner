// SubmissionManager unit tests — driven synchronously through runOnce() with a scripted
// transport, the in-memory FakeJournal, and fully controlled clocks (no sleeping, no I/O).

#include <deque>
#include <string>
#include <vector>

#include "FakeJournal.h"
#include "submit/SubmissionManager.h"
#include "test_framework.h"

using treeminer::CircuitBreaker;
using treeminer::FindKind;
using treeminer::FindStatus;
using treeminer::FoundPayload;
using treeminer::ITransport;
using treeminer::SubmissionManager;
using treeminer::TransportResult;
using treeminer_test::FakeJournal;
using StepResult = SubmissionManager::StepResult;

namespace {

FoundPayload payload(const std::string& key, FindKind kind, std::uint32_t m) {
    FoundPayload p;
    p.key = key;
    p.hash_to_verify = "$argon2id$v=19$m=" + std::to_string(m) + ",t=1,p=1$saltsalt$XEN11digest";
    p.account = "0x1111111111111111111111111111111111111111";
    p.kind = kind;
    p.memory_cost = m;
    p.worker = "w1";
    p.attempts = 1000;
    p.hashes_per_second = 1234.5;
    p.found_at_utc = "2026-01-01T00:00:00Z";
    return p;
}

TransportResult ok(int status, std::string body) {
    TransportResult r;
    r.transport_ok = true;
    r.http_status = status;
    r.body = std::move(body);
    return r;
}

TransportResult down() {
    TransportResult r;
    r.transport_ok = false;
    r.error = "connect refused";
    return r;
}

class FakeTransport : public ITransport {
public:
    TransportResult submit(const FoundPayload& p) override {
        submitted_keys.push_back(p.key);
        return take_(submit_queue);
    }
    TransportResult confirm(const std::string& key) override {
        confirmed_keys.push_back(key);
        return take_(confirm_queue);
    }
    TransportResult difficulty() override {
        ++difficulty_calls;
        return take_(difficulty_queue);
    }

    std::deque<TransportResult> submit_queue, confirm_queue, difficulty_queue;
    std::vector<std::string> submitted_keys, confirmed_keys;
    int difficulty_calls = 0;

private:
    static TransportResult take_(std::deque<TransportResult>& q) {
        if (q.empty()) {
            return down();
        }
        TransportResult r = q.front();
        q.pop_front();
        return r;
    }
};

struct Clocks {
    std::int64_t mono = 0;
    std::int64_t wall = 1767225600000LL;  // 2026-01-01T00:00:00Z (minute 0: XUNI window OPEN)
    void advance(std::int64_t ms) {
        mono += ms;
        wall += ms;
    }
};

SubmissionManager::Config testConfig() {
    SubmissionManager::Config cfg;
    cfg.backoff_base_ms = 2000;
    return cfg;
}

const char* kOk200 = R"({"message": "Hash verified successfully and block saved."})";
const char* kDup400 = R"({"message": "Block already exists, continue"})";
const char* kBlockRow =
    R"({"account": "0x1111111111111111111111111111111111111111", "block_id": 7, "created_at": "2026-01-01 00:00:00", "hash_to_verify": "x", "key": "k"})";

}  // namespace

int main() {
    // --- pure time helpers ---
    TEST_CASE("isoUtc formats epoch ms as ISO-8601 UTC");
    {
        CHECK_STREQ(SubmissionManager::isoUtc(0), "1970-01-01T00:00:00Z");
        CHECK_STREQ(SubmissionManager::isoUtc(784111777000LL), "1994-11-06T08:49:37Z");
        CHECK_STREQ(SubmissionManager::isoUtc(1767225600000LL), "2026-01-01T00:00:00Z");
    }

    TEST_CASE("parseHttpDateMs parses IMF-fixdate");
    {
        auto ms = SubmissionManager::parseHttpDateMs("Sun, 06 Nov 1994 08:49:37 GMT");
        CHECK(ms.has_value());
        CHECK_EQ(ms.value_or(0), 784111777000LL);
        CHECK(!SubmissionManager::parseHttpDateMs("not a date").has_value());
        CHECK(!SubmissionManager::parseHttpDateMs("Sun, 06 Nov 1994 08:49:37 PST").has_value());
    }

    TEST_CASE("xuniWindowAt models the :55-:05 server window");
    {
        // minute 56 -> open, closes at :05 past the next hour.
        auto w = SubmissionManager::xuniWindowAt(56LL * 60000LL);
        CHECK(w.open);
        CHECK_EQ(w.ms_until_close, 4LL * 60000LL + 5LL * 60000LL);
        // minute 3 -> open, closes at :05.
        w = SubmissionManager::xuniWindowAt(3LL * 60000LL);
        CHECK(w.open);
        CHECK_EQ(w.ms_until_close, 2LL * 60000LL);
        // minute 30 -> closed.
        w = SubmissionManager::xuniWindowAt(30LL * 60000LL);
        CHECK(!w.open);
        // boundary: exactly :55 open, exactly :05 closed.
        CHECK(SubmissionManager::xuniWindowAt(55LL * 60000LL).open);
        CHECK(!SubmissionManager::xuniWindowAt(5LL * 60000LL).open);
    }

    // --- happy path: 200 + /get_block confirmation -> Acked ---
    TEST_CASE("200 with confirmed lookup becomes Acked");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("aa11", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Acked);
        CHECK(j.record(id).confirmed_at.has_value());
        CHECK_EQ(t.confirmed_keys.size(), static_cast<std::size_t>(1));
        CHECK_STREQ(t.confirmed_keys[0], "aa11");
        CHECK_EQ(m.metrics().acked, 1u);
        CHECK_EQ(m.metrics().reconciled_via_get_block, 1u);
    }

    // --- the lying-200: confirm 404 -> back to Pending ---
    TEST_CASE("200 with absent lookup is resubmitted (lying-200)");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("aa22", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(404, R"({"error": "Data not found for provided key"})"));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Pending);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().lying_200_detected, 1u);
        // Second pass (after backoff) resubmits and this time it sticks.
        clk.advance(10000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Acked);
        CHECK_EQ(m.metrics().submitted, 2u);
        CHECK_EQ(m.metrics().resubmitted, 1u);
    }

    // --- duplicate + lookup -> Acked ---
    TEST_CASE("duplicate response confirms via lookup to Acked");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("aa33", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(400, kDup400));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Acked);
    }

    // --- lookup unavailable -> stays AcceptedUnconfirmed ---
    TEST_CASE("unavailable lookup leaves AcceptedUnconfirmed");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("aa44", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(down());
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK_EQ(m.metrics().accepted_unconfirmed, 1u);
        CHECK(j.record(id).status_reason.find("unavailable") != std::string::npos);
        // v1.1: a backed-off next_attempt_at is persisted so fetchAwaitingConfirmation
        // re-drives this record later (and does not hot-loop it right now).
        CHECK_STREQ(j.record(id).next_attempt_at.value_or(""),
                    SubmissionManager::isoUtc(clk.wall + 2000));
        CHECK_EQ(t.confirmed_keys.size(), static_cast<std::size_t>(1));
    }

    // --- difficulty hint propagation from a 401 body ---
    TEST_CASE("401 difficulty parks and propagates the m= hint");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        std::vector<std::uint32_t> hints;
        m.setDifficultyHintCallback([&](std::uint32_t d) { hints.push_back(d); });
        auto id = j.append(payload("aa55", FindKind::XEN11, 100000));
        t.submit_queue.push_back(
            ok(401, R"({"message": "Hash does not contain 'm=104000'. Your memory_cost setting in your miner will be autoadjusted."})"));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::ParkedDifficulty);
        CHECK_EQ(hints.size(), static_cast<std::size_t>(1));
        CHECK_EQ(hints.empty() ? 0 : hints[0], 104000u);
        CHECK_EQ(m.lastObservedDifficulty().value_or(0), 104000u);
        CHECK(!j.difficulty_log.empty());
        CHECK_EQ(j.difficulty_log.back().first, 104000u);
    }

    // --- 429 Retry-After drives next_attempt_at ---
    TEST_CASE("429 Retry-After is honored in next_attempt_at");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("aa66", FindKind::XEN11, 100000));
        TransportResult r = ok(429, R"({"message": "slow down"})");
        r.retry_after = "30";
        t.submit_queue.push_back(r);
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Pending);
        CHECK_STREQ(j.record(id).next_attempt_at.value_or(""),
                    SubmissionManager::isoUtc(clk.wall + 30000));
    }

    // --- server clock offset from the Date header ---
    TEST_CASE("Date headers feed the server clock offset");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        CHECK(!m.serverClockOffsetMs().has_value());
        auto id = j.append(payload("aa77", FindKind::XEN11, 100000));
        TransportResult r = ok(200, kOk200);
        // Server is 90 s ahead of our wall clock.
        r.date_header = SubmissionManager::isoUtc(0);  // placeholder, replaced below
        r.date_header = "Thu, 01 Jan 2026 00:01:30 GMT";
        t.submit_queue.push_back(r);
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(m.serverClockOffsetMs().has_value());
        CHECK_EQ(m.serverClockOffsetMs().value_or(0), 90000LL);
        (void)id;
    }

    // --- outage: breaker opens, /difficulty probes, half-open drains, recovery at 1/s ---
    TEST_CASE("outage opens the breaker; recovery closes it through a real submission");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // 2026-01-01T00:30:00Z — XUNI window closed
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("bb11", FindKind::XEN11, 100000));

        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                clk.advance(60000);  // clear per-record backoff and pacing
            }
            t.submit_queue.push_back(down());
            CHECK(m.runOnce() == StepResult::Submitted);
        }
        CHECK(m.breakerState() == CircuitBreaker::State::Open);
        CHECK_EQ(m.metrics().transport_failures, 3u);

        // OPEN: no /verify traffic; probe not due yet right after opening.
        CHECK(m.runOnce() == StepResult::Idle);
        CHECK_EQ(t.submitted_keys.size(), static_cast<std::size_t>(3));

        // Failed probe escalates; successful probe arms HALF_OPEN.
        clk.advance(6000);
        t.difficulty_queue.push_back(down());
        CHECK(m.runOnce() == StepResult::Probed);
        CHECK(m.breakerState() == CircuitBreaker::State::Open);
        clk.advance(11000);
        t.difficulty_queue.push_back(ok(200, R"({"difficulty": "100000"})"));
        CHECK(m.runOnce() == StepResult::Probed);
        CHECK(m.breakerState() == CircuitBreaker::State::HalfOpen);
        CHECK_EQ(m.lastObservedDifficulty().value_or(0), 100000u);

        // HALF_OPEN: one real queued submission; success closes and drain restarts at 1/s.
        clk.advance(60000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(m.breakerState() == CircuitBreaker::State::Closed);
        CHECK(j.record(id).status == FindStatus::Acked);
        CHECK_EQ(static_cast<int>(m.drainRatePerSecond()), 1);
    }

    // --- pacing gate between submissions ---
    TEST_CASE("adaptive pacing gates back-to-back submissions");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // window closed; XEN11 only
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        j.append(payload("cc11", FindKind::XEN11, 100000));
        j.append(payload("cc22", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        // Immediately after: pacing gate holds (rate is finite).
        CHECK(m.runOnce() == StepResult::Idle);
        clk.advance(1000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK_EQ(t.submitted_keys.size(), static_cast<std::size_t>(2));
    }

    // --- XUNI window transition unparks parked XUNI ---
    TEST_CASE("window opening unparks XUNI within budget");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // 00:30 — closed
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        CHECK(m.runOnce() == StepResult::Idle);  // closed window: no unpark
        CHECK_EQ(j.unpark_xuni_calls, 0);
        clk.wall = 1767228900000LL;  // 00:55 — window opens
        CHECK(m.runOnce() == StepResult::Idle);
        CHECK_EQ(j.unpark_xuni_calls, 1);
        CHECK(m.runOnce() == StepResult::Idle);  // still open: no re-trigger
        CHECK_EQ(j.unpark_xuni_calls, 1);
    }

    // --- falling difficulty unparks difficulty-parked records ---
    TEST_CASE("observed difficulty decrease unparks records");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        // The FIRST observation un-parks even though there is no trend yet. A process that
        // just restarted has last_difficulty_ empty, and records parked before the restart
        // may already be valid again at the current floor; waiting for a strict decrease
        // could strand them indefinitely while difficulty trends upward.
        m.observeDifficulty(104000);
        CHECK(m.difficultyTrend() == treeminer::DifficultyTrend::Unknown);
        CHECK_EQ(j.unpark_difficulty_calls.size(), static_cast<std::size_t>(1));
        CHECK_EQ(j.unpark_difficulty_calls[0], 104000u);

        // A rise cannot re-qualify anything, so it must not touch the journal.
        m.observeDifficulty(106000);
        CHECK(m.difficultyTrend() == treeminer::DifficultyTrend::Rising);
        CHECK_EQ(j.unpark_difficulty_calls.size(), static_cast<std::size_t>(1));

        // A fall re-qualifies every parked record with m >= the new floor.
        m.observeDifficulty(100000);
        CHECK(m.difficultyTrend() == treeminer::DifficultyTrend::Falling);
        CHECK_EQ(j.unpark_difficulty_calls.size(), static_cast<std::size_t>(2));
        CHECK_EQ(j.unpark_difficulty_calls[1], 100000u);
    }

    // --- confirmation-retry drain (contract v1.1: fetchAwaitingConfirmation) ---
    TEST_CASE("confirmation retry succeeds -> Acked");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // 00:30 — XUNI window closed
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("dd11", FindKind::XEN11, 100000));
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;  // earlier 200, lookup failed
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);  // no Pending work: retry only
        CHECK(j.record(id).status == FindStatus::Acked);
        CHECK(j.record(id).confirmed_at.has_value());
        CHECK(t.submitted_keys.empty());  // never re-POSTs an unconfirmed record
        CHECK_EQ(m.metrics().confirmation_retries, 1u);
        CHECK_EQ(m.metrics().reconciled_via_get_block, 1u);
    }

    TEST_CASE("confirmation retry finds 404 -> Pending (lying-200 caught late)");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("dd22", FindKind::XEN11, 100000));
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;
        t.confirm_queue.push_back(ok(404, R"({"error": "Data not found for provided key"})"));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::Pending);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().lying_200_detected, 1u);
        // After the backoff it re-enters the normal submission drain.
        clk.advance(10000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Acked);
    }

    TEST_CASE("confirmation retry transport-down stays unconfirmed with future backoff");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("dd33", FindKind::XEN11, 100000));
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;
        t.confirm_queue.push_back(down());
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK_STREQ(j.record(id).next_attempt_at.value_or(""),
                    SubmissionManager::isoUtc(clk.wall + 2000));
        CHECK_EQ(t.confirmed_keys.size(), static_cast<std::size_t>(1));
        // Backoff holds: an immediate second step issues no further lookups.
        CHECK(m.runOnce() == StepResult::Idle);
        CHECK_EQ(t.confirmed_keys.size(), static_cast<std::size_t>(1));
        // Past the backoff the retry lands and the record confirms.
        clk.advance(3000);
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::Acked);
    }

    TEST_CASE("breaker OPEN suppresses confirmation retries");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        j.append(payload("ee11", FindKind::XEN11, 100000));
        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                clk.advance(60000);
            }
            t.submit_queue.push_back(down());
            CHECK(m.runOnce() == StepResult::Submitted);
        }
        CHECK(m.breakerState() == CircuitBreaker::State::Open);
        // An unconfirmed record eligible right now...
        auto u = j.append(payload("ee22", FindKind::XEN11, 100000));
        j.find_(u)->status = FindStatus::AcceptedUnconfirmed;
        // ...is NOT probed while the breaker is OPEN (probe isn't due yet either).
        CHECK(m.runOnce() == StepResult::Idle);
        CHECK(t.confirmed_keys.empty());
        // Once the breaker recovers, the confirmation retry drains again.
        clk.advance(6000);
        t.difficulty_queue.push_back(ok(200, R"({"difficulty": "100000"})"));
        CHECK(m.runOnce() == StepResult::Probed);  // HALF_OPEN now
        clk.advance(60000);
        t.submit_queue.push_back(ok(200, kOk200));   // half-open probe: the Pending record
        t.confirm_queue.push_back(ok(200, kBlockRow));
        t.confirm_queue.push_back(ok(200, kBlockRow));  // then the retry for ee22
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(u).status == FindStatus::Acked);
    }

    // --- head-of-line blocking: neither kind may starve the other out of the fetch slice ---
    TEST_CASE("XEN11 drains past a deep closed-window XUNI backlog");
    {
        // Regression: fetchEligible used to return one mixed oldest-first LIMIT slice. With
        // fetch_limit (16) or more XUNI journaled ahead of a XEN11 — normal after any outage
        // that spans a :55-:05 window — the slice was all-XUNI, none of it selectable while
        // the window is closed, and the XEN11 sat undelivered until the next window (up to
        // ~50 minutes) against a perfectly healthy server.
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // 00:30 — XUNI window CLOSED
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });

        // 20 XUNI (> fetch_limit of 16) journaled BEFORE the one XEN11: worst-case ordering.
        for (int i = 0; i < 20; ++i) {
            char key[8];
            std::snprintf(key, sizeof(key), "xu%02d", i);
            j.append(payload(key, FindKind::XUNI, 100000));
        }
        j.append(payload("xen1", FindKind::XEN11, 100000));

        t.submit_queue.push_back(ok(200, "{\"message\": \"Block added\"}"));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);  // was Idle before the fix
        CHECK_EQ(t.submitted_keys.size(), static_cast<std::size_t>(1));
        CHECK_EQ(t.submitted_keys[0], std::string("xen1"));
        // The closed-window XUNI stayed Pending — parked-in-place, not starved and not dead.
        CHECK_EQ(j.counts().pending, static_cast<std::size_t>(20));
    }

    TEST_CASE("closing window: XUNI preempts past a deep XEN11 backlog");
    {
        // The symmetric direction: a XEN11 backlog deeper than fetch_limit used to hide any
        // XUNI from the slice entirely, so the preemption rule ("XUNI cannot wait near the
        // window's end", PLAN §10.5) had nothing to act on and the XUNI aged out.
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767225600000LL + 4 * 60 * 1000;  // 00:04 — window open, closes at :05
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });

        for (int i = 0; i < 20; ++i) {
            char key[8];
            std::snprintf(key, sizeof(key), "xe%02d", i);
            j.append(payload(key, FindKind::XEN11, 100000));
        }
        j.append(payload("xuni1", FindKind::XUNI, 100000));

        t.submit_queue.push_back(ok(200, "{\"message\": \"Block added\"}"));
        t.confirm_queue.push_back(ok(200, kBlockRow));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK_EQ(t.submitted_keys.size(), static_cast<std::size_t>(1));
        // With <=60 s to the window close, the XUNI goes first even though 20 older XEN11
        // exist — they remain valid after :05; the XUNI does not.
        CHECK_EQ(t.submitted_keys[0], std::string("xuni1"));
    }

    return testfw::summary("submission_manager");
}
