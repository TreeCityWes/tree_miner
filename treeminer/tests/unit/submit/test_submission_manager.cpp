// SubmissionManager unit tests — driven synchronously through runOnce() with a scripted
// transport, the in-memory FakeJournal, and fully controlled clocks (no sleeping, no I/O;
// the one threaded test below waits on a future the fatal callback fulfills).

#include <chrono>
#include <deque>
#include <future>
#include <string>
#include <vector>

#include "FakeJournal.h"
#include "journal/FindJournal.h"  // JournalError only (header-only; nothing linked)
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

// The realistic /get_block 200 body for a given record: gpage.py:331-364 returns the
// stored row itself, so the key and hash_to_verify must be THIS record's values. The
// manager now validates that (finding 4), so a fixture with placeholder key "k" would be
// rejected exactly like an attacker's fabricated 200 — the fixture must earn the ack.
std::string blockRow(const FoundPayload& p) {
    return std::string(R"({"account": ")") + p.account +
           R"(", "block_id": 7, "created_at": "2026-01-01 00:00:00", "hash_to_verify": ")" +
           p.hash_to_verify + R"(", "key": ")" + p.key + R"("})";
}

// FakeJournal whose recordAttempt fails like a broken SQLite volume would: the drain
// thread's exception boundary (finding 8) must contain this, not the process.
class ThrowingJournal : public FakeJournal {
public:
    void recordAttempt(std::int64_t, const treeminer::Classification&, std::optional<int>,
                       const std::string&, const std::optional<std::string>&,
                       const std::string&) override {
        ++throw_count;
        throw treeminer::JournalError("disk I/O error (simulated)");
    }
    int throw_count = 0;
};

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
        std::vector<FindStatus> outcomes;
        m.setOutcomeCallback([&](const auto&, const auto& classification, auto) {
            outcomes.push_back(classification.next_status);
        });
        const auto p = payload("aa11", FindKind::XEN11, 100000);
        auto id = j.append(p);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Acked);
        CHECK(j.record(id).confirmed_at.has_value());
        CHECK_EQ(t.confirmed_keys.size(), static_cast<std::size_t>(1));
        CHECK_STREQ(t.confirmed_keys[0], "aa11");
        CHECK_EQ(m.metrics().acked, 1u);
        CHECK_EQ(m.metrics().reconciled_via_get_block, 1u);
        CHECK_EQ(outcomes.size(), static_cast<std::size_t>(1));
        CHECK(outcomes.front() == FindStatus::Acked);
    }

    // --- the unconfirmed-200: confirm 404 -> back to Pending ---
    TEST_CASE("200 with absent lookup is resubmitted (unconfirmed-200)");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        const auto p = payload("aa22", FindKind::XEN11, 100000);
        auto id = j.append(p);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(404, R"({"error": "Data not found for provided key"})"));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::Pending);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().unconfirmed_200_detected, 1u);
        // Second pass (after backoff) resubmits and this time it sticks.
        clk.advance(10000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
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
        const auto p = payload("aa33", FindKind::XEN11, 100000);
        auto id = j.append(p);
        t.submit_queue.push_back(ok(400, kDup400));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
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
        std::vector<FindStatus> outcomes;
        m.setDifficultyHintCallback([&](std::uint32_t d) { hints.push_back(d); });
        m.setOutcomeCallback([&](const auto&, const auto& classification, auto) {
            outcomes.push_back(classification.next_status);
        });
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
        CHECK_EQ(outcomes.size(), static_cast<std::size_t>(1));
        CHECK(outcomes.front() == FindStatus::ParkedDifficulty);
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
        const auto p = payload("aa77", FindKind::XEN11, 100000);
        auto id = j.append(p);
        TransportResult r = ok(200, kOk200);
        // Server is 90 s ahead of our wall clock.
        r.date_header = SubmissionManager::isoUtc(0);  // placeholder, replaced below
        r.date_header = "Thu, 01 Jan 2026 00:01:30 GMT";
        t.submit_queue.push_back(r);
        t.confirm_queue.push_back(ok(200, blockRow(p)));
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
        std::vector<CircuitBreaker::State> network_states;
        m.setNetworkStateCallback(
            [&](CircuitBreaker::State state) { network_states.push_back(state); });
        const auto p = payload("bb11", FindKind::XEN11, 100000);
        auto id = j.append(p);

        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                clk.advance(60000);  // clear per-record backoff and pacing
            }
            t.submit_queue.push_back(down());
            CHECK(m.runOnce() == StepResult::Submitted);
        }
        CHECK(m.breakerState() == CircuitBreaker::State::Open);
        CHECK(!network_states.empty());
        CHECK(network_states.back() == CircuitBreaker::State::Open);
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
        CHECK(network_states.back() == CircuitBreaker::State::HalfOpen);
        CHECK_EQ(m.lastObservedDifficulty().value_or(0), 100000u);

        // HALF_OPEN: one real queued submission; success closes and drain restarts at 1/s.
        clk.advance(60000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(m.breakerState() == CircuitBreaker::State::Closed);
        CHECK(network_states.back() == CircuitBreaker::State::Closed);
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
        const auto p1 = payload("cc11", FindKind::XEN11, 100000);
        const auto p2 = payload("cc22", FindKind::XEN11, 100000);
        j.append(p1);
        j.append(p2);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p1)));
        CHECK(m.runOnce() == StepResult::Submitted);
        // Immediately after: pacing gate holds (rate is finite).
        CHECK(m.runOnce() == StepResult::Idle);
        clk.advance(1000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p2)));
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

    TEST_CASE("closed-window XUNI backlog does not hide a later XEN11");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // 00:30 - XUNI window closed
        auto cfg = testConfig();
        cfg.fetch_limit = 2;
        SubmissionManager m(j, t, cfg, [&] { return clk.mono; },
                            [&] { return clk.wall; });
        j.append(payload("xuni-1", FindKind::XUNI, 100000));
        j.append(payload("xuni-2", FindKind::XUNI, 100000));
        j.append(payload("xuni-3", FindKind::XUNI, 100000));
        const auto xen = payload("xen-1", FindKind::XEN11, 100000);
        const auto xen_id = j.append(xen);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(xen)));

        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK_STREQ(t.submitted_keys.front(), "xen-1");
        CHECK(j.record(xen_id).status == FindStatus::Acked);
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
        const auto p = payload("dd11", FindKind::XEN11, 100000);
        auto id = j.append(p);
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;  // earlier 200, lookup failed
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);  // no Pending work: retry only
        CHECK(j.record(id).status == FindStatus::Acked);
        CHECK(j.record(id).confirmed_at.has_value());
        CHECK(t.submitted_keys.empty());  // never re-POSTs an unconfirmed record
        CHECK_EQ(m.metrics().confirmation_retries, 1u);
        CHECK_EQ(m.metrics().reconciled_via_get_block, 1u);
    }

    TEST_CASE("confirmation retry finds 404 -> Pending (unconfirmed-200 caught late)");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        const auto p = payload("dd22", FindKind::XEN11, 100000);
        auto id = j.append(p);
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;
        t.confirm_queue.push_back(ok(404, R"({"error": "Data not found for provided key"})"));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::Pending);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().unconfirmed_200_detected, 1u);
        // After the backoff it re-enters the normal submission drain.
        clk.advance(10000);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
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
        const auto p = payload("dd33", FindKind::XEN11, 100000);
        auto id = j.append(p);
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
        t.confirm_queue.push_back(ok(200, blockRow(p)));
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
        const auto p1 = payload("ee11", FindKind::XEN11, 100000);
        j.append(p1);
        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                clk.advance(60000);
            }
            t.submit_queue.push_back(down());
            CHECK(m.runOnce() == StepResult::Submitted);
        }
        CHECK(m.breakerState() == CircuitBreaker::State::Open);
        // An unconfirmed record eligible right now...
        const auto p2 = payload("ee22", FindKind::XEN11, 100000);
        auto u = j.append(p2);
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
        t.confirm_queue.push_back(ok(200, blockRow(p1)));
        t.confirm_queue.push_back(ok(200, blockRow(p2)));  // then the retry for ee22
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
        const auto xen = payload("xen1", FindKind::XEN11, 100000);
        j.append(xen);

        t.submit_queue.push_back(ok(200, "{\"message\": \"Block added\"}"));
        t.confirm_queue.push_back(ok(200, blockRow(xen)));
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
        const auto xuni = payload("xuni1", FindKind::XUNI, 100000);
        j.append(xuni);

        t.submit_queue.push_back(ok(200, "{\"message\": \"Block added\"}"));
        t.confirm_queue.push_back(ok(200, blockRow(xuni)));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK_EQ(t.submitted_keys.size(), static_cast<std::size_t>(1));
        // With <=60 s to the window close, the XUNI goes first even though 20 older XEN11
        // exist — they remain valid after :05; the XUNI does not.
        CHECK_EQ(t.submitted_keys[0], std::string("xuni1"));
    }

    // =============================================================================
    // Finding 4 — a confirmation 200 is only an ack when its body proves our row.
    // =============================================================================

    TEST_CASE("confirmationMatches validates key and hash byte-for-byte");
    {
        using Check = SubmissionManager::ConfirmBodyCheck;
        treeminer::FindRecord rec;
        rec.payload = payload("aabb", FindKind::XEN11, 100000);
        // The real row (gpage.py:331-364) confirms; hash_to_verify may legitimately be
        // absent (older rows / XUNI table), and then the key alone decides.
        CHECK(SubmissionManager::confirmationMatches(rec, blockRow(rec.payload)) ==
              Check::Confirmed);
        CHECK(SubmissionManager::confirmationMatches(rec, R"({"key": "aabb"})") ==
              Check::Confirmed);
        // Non-JSON, empty, HTML error pages, arrays, and key-less objects are all
        // Malformed: a body that identifies nothing can confirm nothing.
        CHECK(SubmissionManager::confirmationMatches(rec, "") == Check::Malformed);
        CHECK(SubmissionManager::confirmationMatches(rec, "<html>502</html>") ==
              Check::Malformed);
        CHECK(SubmissionManager::confirmationMatches(rec, "OK") == Check::Malformed);
        CHECK(SubmissionManager::confirmationMatches(rec, R"([{"key": "aabb"}])") ==
              Check::Malformed);
        CHECK(SubmissionManager::confirmationMatches(rec, R"({"block_id": 7})") ==
              Check::Malformed);
        // A different key is some other row — not a confirmation of ours. Byte equality:
        // case differences are mismatches too.
        CHECK(SubmissionManager::confirmationMatches(rec, R"({"key": "ffff"})") ==
              Check::KeyMismatch);
        CHECK(SubmissionManager::confirmationMatches(rec, R"({"key": "AABB"})") ==
              Check::KeyMismatch);
        // Our key with a different stored hash is the serious case.
        CHECK(SubmissionManager::confirmationMatches(
                  rec, R"({"key": "aabb", "hash_to_verify": "$argon2id$other"})") ==
              Check::HashMismatch);
    }

    TEST_CASE("initial confirm: 200 with WRONG key stays AcceptedUnconfirmed, then recovers");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;  // 00:30 — XUNI window closed
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        const auto p = payload("ff11", FindKind::XEN11, 100000);
        const auto other = payload("attacker", FindKind::XEN11, 100000);
        auto id = j.append(p);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(other)));  // 200, but not OUR row
        CHECK(m.runOnce() == StepResult::Submitted);
        // Not Acked (nothing proven), not Pending (server may hold the row): unconfirmed
        // with the normal per-record backoff in the future.
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK_STREQ(j.record(id).next_attempt_at.value_or(""),
                    SubmissionManager::isoUtc(clk.wall + 2000));
        CHECK_EQ(m.metrics().acked, 0u);
        CHECK_EQ(m.metrics().reconciled_via_get_block, 0u);
        CHECK_EQ(m.metrics().confirm_body_rejected, 1u);
        CHECK(j.record(id).status_reason.find("different key") != std::string::npos);
        // The retry path re-asks after the backoff; a genuine row then acks it.
        clk.advance(3000);
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::Acked);
    }

    TEST_CASE("initial confirm: 200 with garbage body stays AcceptedUnconfirmed");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("ff22", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, "<html><body>captive portal</body></html>"));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().acked, 0u);
        CHECK_EQ(m.metrics().confirm_body_rejected, 1u);
    }

    TEST_CASE("initial confirm: 200 JSON missing the key field stays AcceptedUnconfirmed");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        auto id = j.append(payload("ff33", FindKind::XEN11, 100000));
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, R"({"block_id": 7, "account": "0x1111"})"));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().confirm_body_rejected, 1u);
    }

    TEST_CASE("initial confirm: matching key but mismatched hash_to_verify is never acked");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        const auto p = payload("ff44", FindKind::XEN11, 100000);
        auto id = j.append(p);
        auto impostor = p;  // same key, different stored hash: server row is NOT our find
        impostor.hash_to_verify = "$argon2id$v=19$m=100000,t=1,p=1$saltsalt$SOMEONEELSE";
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(impostor)));
        CHECK(m.runOnce() == StepResult::Submitted);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK(j.record(id).next_attempt_at.has_value());
        CHECK_EQ(m.metrics().acked, 0u);
        CHECK_EQ(m.metrics().confirm_body_rejected, 1u);
        CHECK(j.record(id).status_reason.find("DIFFERENT hash_to_verify") !=
              std::string::npos);
    }

    TEST_CASE("confirm retry: 200 with wrong key stays AcceptedUnconfirmed with backoff");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        const auto p = payload("gg11", FindKind::XEN11, 100000);
        const auto other = payload("attacker", FindKind::XEN11, 100000);
        auto id = j.append(p);
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;
        t.confirm_queue.push_back(ok(200, blockRow(other)));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK_STREQ(j.record(id).next_attempt_at.value_or(""),
                    SubmissionManager::isoUtc(clk.wall + 2000));
        CHECK_EQ(m.metrics().acked, 0u);
        CHECK_EQ(m.metrics().confirm_body_rejected, 1u);
        // Past the backoff, a genuine row still acks it.
        clk.advance(3000);
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::Acked);
    }

    TEST_CASE("confirm retry: garbage and hash-mismatch 200s stay AcceptedUnconfirmed");
    {
        FakeJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        const auto p = payload("gg22", FindKind::XEN11, 100000);
        auto id = j.append(p);
        j.find_(id)->status = FindStatus::AcceptedUnconfirmed;
        t.confirm_queue.push_back(ok(200, "not json at all"));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK(j.record(id).next_attempt_at.has_value());
        // And the serious flavor on the retry path too: same key, different hash.
        auto impostor = p;
        impostor.hash_to_verify = "$argon2id$v=19$m=100000,t=1,p=1$saltsalt$SOMEONEELSE";
        clk.advance(3000);
        t.confirm_queue.push_back(ok(200, blockRow(impostor)));
        CHECK(m.runOnce() == StepResult::ConfirmRetried);
        CHECK(j.record(id).status == FindStatus::AcceptedUnconfirmed);
        CHECK_EQ(m.metrics().acked, 0u);
        CHECK_EQ(m.metrics().confirm_body_rejected, 2u);
    }

    // =============================================================================
    // Finding 8 — exceptions inside the drain step must never escape (std::terminate).
    // =============================================================================

    TEST_CASE("journal exception is contained: fatal callback fires once, loop goes inert");
    {
        ThrowingJournal j;
        FakeTransport t;
        Clocks clk;
        clk.wall = 1767227400000LL;
        SubmissionManager m(j, t, testConfig(), [&] { return clk.mono; },
                            [&] { return clk.wall; });
        std::vector<std::string> fatals;
        m.setFatalCallback([&](const std::string& what) { fatals.push_back(what); });
        const auto p = payload("hh11", FindKind::XEN11, 100000);
        j.append(p);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        bool escaped = false;
        StepResult r = StepResult::Submitted;
        try {
            r = m.runOnce();  // recordAttempt throws JournalError inside
        } catch (...) {
            escaped = true;
        }
        CHECK(!escaped);
        CHECK(r == StepResult::Idle);
        CHECK_EQ(j.throw_count, 1);
        CHECK_EQ(fatals.size(), static_cast<std::size_t>(1));
        CHECK(fatals[0].find("disk I/O error") != std::string::npos);
        CHECK_EQ(m.metrics().thread_loop_exceptions, 1u);
        // First exception wins: the manager is inert now — no further journal/transport
        // work, no second callback.
        clk.advance(60000);
        CHECK(m.runOnce() == StepResult::Idle);
        CHECK_EQ(j.throw_count, 1);
        CHECK_EQ(fatals.size(), static_cast<std::size_t>(1));
        // stop() is safe even though start() was never called.
        m.stop();
    }

    TEST_CASE("thread loop survives a journal exception and stop() joins cleanly");
    {
        // The one threaded test: the real threadLoop_ hits the throwing journal on its
        // first step, halts itself, and stays joinable. The fatal callback fulfills a
        // promise, so the wait is event-driven — no sleeps, no polling.
        ThrowingJournal j;
        FakeTransport t;
        SubmissionManager m(j, t, testConfig());  // default (real) clocks: fine, the
                                                  // fatal fires on the very first step
        std::promise<std::string> fatal_promise;
        auto fatal_future = fatal_promise.get_future();
        m.setFatalCallback(
            [&](const std::string& what) { fatal_promise.set_value(what); });
        const auto p = payload("hh22", FindKind::XEN11, 100000);
        j.append(p);
        t.submit_queue.push_back(ok(200, kOk200));
        t.confirm_queue.push_back(ok(200, blockRow(p)));
        m.start();
        CHECK(fatal_future.wait_for(std::chrono::seconds(10)) ==
              std::future_status::ready);
        // The loop halted itself (running_ cleared by handleFatal_); stop() must still
        // join the finished-but-joinable thread instead of early-returning past it.
        m.stop();
        CHECK_EQ(m.metrics().thread_loop_exceptions, 1u);
        CHECK_EQ(j.throw_count, 1);
    }

    return testfw::summary("submission_manager");
}
