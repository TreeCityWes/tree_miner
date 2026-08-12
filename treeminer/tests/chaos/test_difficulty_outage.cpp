// Chaos test — PLAN.md §6 case 3: "difficulty +3000 during outage => affected finds park,
// then auto-submit when lowered."
//
// This drives the REAL FindJournal (SQLite, WAL, synchronous=FULL on a temp file) and the
// REAL SubmissionManager against a scripted server model. Only the transport is faked, so
// the durability guarantee, the response classification, the park/unpark transitions and the
// drain pacing are all exercised for real. No GPU, no network, no sleeping: the manager's
// clocks are injected, which is what makes a multi-hour outage testable in milliseconds.
//
// The server model mirrors gpage.py where it matters:
//   - rejection is strictly `submitted_m < current_difficulty` (gpage.py:404,412)
//   - the 401 body embeds the CURRENT difficulty as m={N} (gpage.py:416)
//   - a stored key answers /get_block, an absent one 404s (gpage.py:331-364)

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "journal/FindJournal.h"
#include "submit/SubmissionManager.h"
#include "treeminer/MarginPolicy.h"
#include "treeminer/Types.h"

#include "test_framework.h"

using namespace treeminer;

namespace {

// ---------------------------------------------------------------- scripted server model

class ScriptedServer : public ITransport {
public:
    explicit ScriptedServer(std::uint32_t difficulty) : difficulty_(difficulty) {}

    // --- chaos controls ---
    void takeDown() { up_ = false; }
    void bringUp() { up_ = true; }
    void setDifficulty(std::uint32_t d) { difficulty_ = d; }
    std::uint32_t difficultyValue() const { return difficulty_; }
    std::size_t storedCount() const { return stored_.size(); }
    int verifyRequests() const { return verify_requests_; }

    TransportResult submit(const FoundPayload& payload) override {
        ++verify_requests_;
        if (!up_) {
            return down_();
        }
        // gpage.py:404,412 — strictly-less-than, so m == difficulty is accepted.
        if (payload.memory_cost < difficulty_) {
            TransportResult r;
            r.transport_ok = true;
            r.http_status = 401;
            r.body = "{\"message\": \"Hash does not contain 'm=" +
                     std::to_string(difficulty_) + "'. Current difficulty is " +
                     std::to_string(difficulty_) + "\"}";
            return r;
        }
        stored_[payload.key] = payload.hash_to_verify;
        TransportResult r;
        r.transport_ok = true;
        r.http_status = 200;
        r.body = "{\"message\": \"Block added\"}";
        return r;
    }

    TransportResult confirm(const std::string& key) override {
        if (!up_) {
            return down_();
        }
        TransportResult r;
        r.transport_ok = true;
        const bool found = stored_.count(key) != 0;
        r.http_status = found ? 200 : 404;
        // gpage.py:331-364 returns the stored row itself. The manager validates that the
        // 200 body's key (and hash_to_verify, when present) matches the record it asked
        // about (security finding 4), so the model must echo the REAL stored values —
        // a bare 200 would no longer count as confirmation.
        r.body = found ? "{\"key\": \"" + key + "\", \"hash_to_verify\": \"" +
                             stored_[key] + "\"}"
                       : "{\"error\": \"not found\"}";
        return r;
    }

    TransportResult difficulty() override {
        if (!up_) {
            return down_();
        }
        TransportResult r;
        r.transport_ok = true;
        r.http_status = 200;
        r.body = "{\"difficulty\": \"" + std::to_string(difficulty_) + "\"}";
        return r;
    }

private:
    static TransportResult down_() {
        TransportResult r;
        r.transport_ok = false;
        r.error = "connection refused (chaos: server down)";
        return r;
    }

    bool up_ = true;
    std::uint32_t difficulty_;
    std::map<std::string, std::string> stored_;
    int verify_requests_ = 0;
};

// ---------------------------------------------------------------- harness helpers

// A journal on a scratch file, removed when the test object dies. The real SQLite path is
// the point: an in-memory fake would not prove the finds survive at all.
class TempJournal {
public:
    explicit TempJournal(const char* tag) {
        path_ = std::string("treeminer-chaos-") + tag + ".db";
        std::remove(path_.c_str());
        journal_ = new FindJournal(path_);
    }
    ~TempJournal() {
        delete journal_;
        std::remove(path_.c_str());
        std::remove((path_ + "-wal").c_str());
        std::remove((path_ + "-shm").c_str());
    }
    FindJournal& get() { return *journal_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    FindJournal* journal_ = nullptr;
};

FoundPayload makeFind(const std::string& key, std::uint32_t m, FindKind kind = FindKind::XEN11) {
    FoundPayload p;
    p.key = key;
    // The PHC string is assembled once at discovery from the batch's actual memory cost —
    // this test never recomputes it, exactly like the miner (PLAN §10.1).
    p.hash_to_verify = "$argon2id$v=19$m=" + std::to_string(m) +
                       ",t=1,p=1$WEVOMTAwODIwMjJYRU4$" + key.substr(0, 16) + "XEN11abcdefgh";
    p.account = "0x1234567890123456789012345678901234567890";
    p.kind = kind;
    p.memory_cost = m;
    p.worker = "chaos";
    p.attempts = 1;
    p.hashes_per_second = 1000.0;
    p.found_at_utc = "2026-08-10T00:00:00Z";
    return p;
}

// Counts by status, read straight from the journal.
IFindJournal::Counts countsOf(FindJournal& j) { return j.counts(); }

// Drive the manager for a bounded number of steps, advancing the injected clock as the real
// thread loop would. Time MUST advance: drain pacing, per-record backoff and breaker probe
// scheduling are all clock-gated, so a frozen clock would let exactly one submission through
// and then report "idle" forever.
void pump(SubmissionManager& mgr, std::int64_t& now_ms, int steps,
          std::int64_t step_ms = 250 /* matches Config::idle_poll_ms */) {
    for (int i = 0; i < steps; ++i) {
        mgr.runOnce();
        now_ms += step_ms;
    }
}

}  // namespace

// =====================================================================================

int main() {
    // Injected clocks: `now_ms` is advanced by hand so outages and per-record backoff can be
    // fast-forwarded. Both the monotonic and wall clocks read it, so persisted
    // next_attempt_at values stay consistent with the pacing decisions.
    std::int64_t now_ms = 1767225600000LL;  // 2026-01-01T00:00:00Z, mid-hour: XUNI closed
    auto clock = [&now_ms]() { return now_ms; };

    // ---------------------------------------------------------------------------------
    TEST_CASE("difficulty +3000 during outage: finds park, then unpark when it falls back");
    {
        constexpr std::uint32_t kMinedAt = 100000;
        ScriptedServer server(kMinedAt);
        TempJournal journal("park");

        SubmissionManager::Config cfg;
        cfg.margin.mode = MarginMode::Off;  // no headroom: this is the bare park/unpark path
        SubmissionManager mgr(journal.get(), server, cfg, clock, clock);

        // --- 1. Server goes down mid-mining; three finds are made during the outage. ---
        server.takeDown();
        const std::vector<std::string> keys = {
            "aaaa000000000000000000000000000000000000000000000000000000000001",
            "aaaa000000000000000000000000000000000000000000000000000000000002",
            "aaaa000000000000000000000000000000000000000000000000000000000003"};
        for (const auto& k : keys) {
            journal.get().append(makeFind(k, kMinedAt));  // durable before any network attempt
        }
        CHECK_EQ(countsOf(journal.get()).pending, 3u);

        // The submitter tries, fails on transport, and opens the breaker. Nothing is lost.
        pump(mgr, now_ms, 80);  // 20 s: enough failures to trip the breaker
        CHECK(mgr.breakerState() == CircuitBreaker::State::Open);
        CHECK_EQ(countsOf(journal.get()).acked_total, 0u);
        CHECK_EQ(countsOf(journal.get()).pending, 3u);  // still pending, still durable

        // --- 2. Difficulty climbs +3000 while the server is unreachable. ---
        server.setDifficulty(kMinedAt + 3000);

        // --- 3. Server returns. The finds are now below the floor: 401 -> ParkedDifficulty. ---
        now_ms += 10 * 60 * 1000;  // 10 minutes of outage elapse
        server.bringUp();
        pump(mgr, now_ms, 600);  // 150 s: probe recovers, then all three drain

        {
            const auto c = countsOf(journal.get());
            CHECK_EQ(c.parked, 3u);        // parked, NOT dead and NOT dropped
            CHECK_EQ(c.acked_total, 0u);
            CHECK_EQ(c.pending, 0u);
            CHECK_EQ(c.dead_total, 0u);
            CHECK_EQ(c.quarantined, 0u);   // a known 401 must never be quarantined
        }
        // The 401 body carried the current difficulty; the manager adopted it without
        // waiting for the poller (PLAN §10.4).
        CHECK(mgr.lastObservedDifficulty().has_value());
        CHECK_EQ(*mgr.lastObservedDifficulty(), kMinedAt + 3000);

        // --- 4. Difficulty falls back to where the finds were mined. ---
        server.setDifficulty(kMinedAt);
        mgr.observeDifficulty(kMinedAt);  // the poller's next sample: a strict decrease

        {
            const auto c = countsOf(journal.get());
            CHECK_EQ(c.parked, 0u);    // un-parked by the falling floor
            CHECK_EQ(c.pending, 3u);   // and immediately eligible again
        }

        // --- 5. They drain and are confirmed via /get_block. ---
        now_ms += 5 * 60 * 1000;  // clear any per-record backoff
        pump(mgr, now_ms, 600);

        {
            const auto c = countsOf(journal.get());
            CHECK_EQ(c.acked_total, 3u);   // every find recovered
            CHECK_EQ(c.pending, 0u);
            CHECK_EQ(c.parked, 0u);
            CHECK_EQ(c.dead_total, 0u);
            CHECK_EQ(c.permanently_invalid, 0u);
        }
        CHECK_EQ(server.storedCount(), 3u);  // and the server really holds them
    }

    // ---------------------------------------------------------------------------------
    TEST_CASE("auto margin: finds mined during an outage clear the risen floor outright");
    {
        // Same chaos, but with the headroom policy enabled. The point of difficulty_margin
        // is that the +3000 rise never parks anything: the finds were mined with enough
        // memory cost to still be valid when the server comes back.
        constexpr std::uint32_t kBaseDifficulty = 100000;
        ScriptedServer server(kBaseDifficulty);
        TempJournal journal("margin");

        SubmissionManager::Config cfg;
        cfg.margin.mode = MarginMode::Auto;
        cfg.margin.margin_kib = 1000;      // one adjustment period of headroom per step
        cfg.margin.max_kib = 5000;
        cfg.margin_eval_interval_ms = 0;   // re-evaluate every step; the test owns the clock
        SubmissionManager mgr(journal.get(), server, cfg, clock, clock);

        std::uint32_t published_margin = 0;
        mgr.setMarginCallback([&published_margin](std::uint32_t kib) { published_margin = kib; });

        // Healthy: the margin must be zero. Headroom costs hashrate, so a healthy miner
        // must not be paying for insurance it does not need.
        mgr.runOnce();
        CHECK_EQ(mgr.marginInEffect(), 0u);

        // Outage begins. One failing submission is enough to start the breaker climbing;
        // append a find so the manager has something to try.
        server.takeDown();
        journal.get().append(makeFind(
            "bbbb000000000000000000000000000000000000000000000000000000000001", kBaseDifficulty));
        pump(mgr, now_ms, 80);
        CHECK(mgr.breakerState() == CircuitBreaker::State::Open);

        // 20 minutes into the outage the ramp has climbed: 1 immediate step + 4 periods.
        now_ms += 20 * 60 * 1000;
        mgr.runOnce();
        const std::uint32_t margin = mgr.marginInEffect();
        CHECK_EQ(margin, 5000u);            // capped at max_kib
        CHECK_EQ(published_margin, margin);  // and the mine loop was told

        // The mine loop mines at difficulty + margin (MineUnit uses effectiveMiningDifficulty).
        const std::uint32_t mined_m = kBaseDifficulty + margin;
        journal.get().append(makeFind(
            "bbbb000000000000000000000000000000000000000000000000000000000002", mined_m));

        // Difficulty rises +3000 during the outage, exactly as in the first case.
        server.setDifficulty(kBaseDifficulty + 3000);
        server.bringUp();
        now_ms += 5 * 60 * 1000;
        pump(mgr, now_ms, 600);

        const auto c = countsOf(journal.get());
        // The headroom find cleared the risen floor and was accepted outright.
        // The bare find (mined before the margin engaged) parked, as it must.
        CHECK_EQ(c.acked_total, 1u);
        CHECK_EQ(c.parked, 1u);
        CHECK_EQ(c.dead_total, 0u);
        CHECK_EQ(c.quarantined, 0u);
    }

    // ---------------------------------------------------------------------------------
    TEST_CASE("parked finds survive a process restart and unpark afterwards");
    {
        // The journal is the durable record, so parking must outlive the process. This is
        // the restart-during-an-outage case an operator actually hits.
        constexpr std::uint32_t kMinedAt = 50000;
        const std::string db = "treeminer-chaos-restart.db";
        std::remove(db.c_str());

        {
            ScriptedServer server(kMinedAt + 3000);  // floor already above the find
            FindJournal journal(db);
            SubmissionManager::Config cfg;
            SubmissionManager mgr(journal, server, cfg, clock, clock);
            journal.append(makeFind(
                "cccc000000000000000000000000000000000000000000000000000000000001", kMinedAt));
            pump(mgr, now_ms, 200);
            CHECK_EQ(journal.counts().parked, 1u);
        }  // process "exits" — journal closed, manager destroyed

        {
            ScriptedServer server(kMinedAt);  // difficulty fell while we were down
            FindJournal journal(db);          // reopened from disk
            const auto recovered = journal.recoverOnStartup();
            CHECK_EQ(recovered.parked_difficulty, 1u);  // the find is still there

            SubmissionManager::Config cfg;
            SubmissionManager mgr(journal, server, cfg, clock, clock);

            // A restart must not strand a parked find that is valid again. The manager
            // learns the current difficulty from its first observation.
            mgr.observeDifficulty(kMinedAt);
            now_ms += 5 * 60 * 1000;
            pump(mgr, now_ms, 300);

            const auto c = journal.counts();
            CHECK_EQ(c.acked_total, 1u);
            CHECK_EQ(c.parked, 0u);
        }

        std::remove(db.c_str());
        std::remove((db + "-wal").c_str());
        std::remove((db + "-shm").c_str());
    }

    return testfw::summary("chaos_difficulty_outage");
}
