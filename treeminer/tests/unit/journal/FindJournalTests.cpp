// FindJournalTests.cpp — standalone unit tests for treeminer::FindJournal.
// No GPU, no network. Each test runs against a fresh SQLite file in the system temp
// directory and is registered as one CTest case (see CMakeLists.txt); running the binary
// with no arguments executes every test.
//
// Tests verify behavior through the public IFindJournal API, and additionally through
// raw SQLite reads of the database file where the contract is about persisted bytes
// (e.g. status strings must match the Types.h enumerator names exactly).

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "journal/FindJournal.h"

namespace {

using treeminer::Classification;
using treeminer::FindJournal;
using treeminer::FindKind;
using treeminer::FindRecord;
using treeminer::FindStatus;
using treeminer::FoundPayload;
using treeminer::IFindJournal;
using treeminer::JournalError;

// ---------------------------------------------------------------------------------------
// Tiny test framework: CHECK/CHECK_EQ throw TestFailure with file:line; a registry maps
// test names to functions so CTest can run each case individually.
// ---------------------------------------------------------------------------------------

struct TestFailure : std::runtime_error {
    explicit TestFailure(const std::string& what) : std::runtime_error(what) {}
};

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +\
                              ": CHECK failed: " #condition);                         \
        }                                                                             \
    } while (0)

template <typename A, typename B>
void checkEqImpl(const A& actual, const B& expected, const char* actualText,
                 const char* expectedText, const char* file, int line) {
    if (!(actual == expected)) {
        std::ostringstream message;
        message << file << ":" << line << ": CHECK_EQ failed: " << actualText
                << " == " << expectedText << " (actual: " << actual
                << ", expected: " << expected << ")";
        throw TestFailure(message.str());
    }
}

#define CHECK_EQ(actual, expected) \
    checkEqImpl((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define CHECK_THROWS(expression, ExceptionType)                                       \
    do {                                                                              \
        bool caught_ = false;                                                         \
        try {                                                                         \
            (void)(expression);                                                       \
        } catch (const ExceptionType&) {                                              \
            caught_ = true;                                                           \
        }                                                                             \
        if (!caught_) {                                                               \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +\
                              ": expected " #expression " to throw " #ExceptionType); \
        }                                                                             \
    } while (0)

std::map<std::string, std::function<void()>>& registry() {
    static std::map<std::string, std::function<void()>> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().emplace(name, std::move(fn));
    }
};

#define TEST_CASE(name)                                        \
    void name();                                               \
    const Registrar registrar_##name(#name, name);             \
    void name()

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

// Fresh database path per test; deletes the db (+ -wal/-shm) on construction and
// destruction so runs are hermetic.
struct TempDb {
    explicit TempDb(const std::string& testName) {
        path = (std::filesystem::temp_directory_path() /
                ("treeminer_journal_test_" + testName + ".db"))
                   .string();
        cleanup();
    }
    ~TempDb() { cleanup(); }

    void cleanup() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }

    std::string path;
};

// Raw read/write access to the database file, independent of FindJournal, for verifying
// persisted bytes and for injecting states the API refuses to write.
struct RawDb {
    explicit RawDb(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) !=
            SQLITE_OK) {
            throw TestFailure("RawDb: cannot open " + path);
        }
        sqlite3_busy_timeout(db, 5000);
    }
    ~RawDb() { sqlite3_close_v2(db); }

    // First column of the first row, as text; nullopt when NULL or no row.
    std::optional<std::string> scalarText(const std::string& sql) const {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            throw TestFailure(std::string("RawDb prepare failed: ") + sqlite3_errmsg(db));
        }
        std::optional<std::string> result;
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        } else if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw TestFailure(std::string("RawDb step failed: ") + sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
        return result;
    }

    std::int64_t scalarInt(const std::string& sql) const {
        const auto text = scalarText(sql);
        if (!text.has_value()) {
            throw TestFailure("RawDb: expected a scalar for: " + sql);
        }
        return std::stoll(*text);
    }

    void exec(const std::string& sql) const {
        char* errorText = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errorText) != SQLITE_OK) {
            std::string message = std::string("RawDb exec failed: ") +
                                  (errorText != nullptr ? errorText : "?");
            sqlite3_free(errorText);
            throw TestFailure(message);
        }
    }

    sqlite3* db = nullptr;
};

FoundPayload makePayload(const std::string& keySuffix, FindKind kind = FindKind::XEN11,
                         std::uint32_t memoryCost = 1727) {
    FoundPayload payload;
    payload.key = "aabbccddeeff00112233445566778899aabbccddeeff001122334455667788" +
                  (keySuffix.size() >= 2 ? keySuffix : "0" + keySuffix);
    payload.hash_to_verify =
        "$argon2id$v=19$m=" + std::to_string(memoryCost) +
        ",t=1,p=1$c29tZXNhbHRzb21lc2FsdDE5$" +
        "TFVYRU4xMWFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6QUJDREVGRw";
    payload.account = "0x1234567890abcdef1234567890abcdef12345678";
    payload.kind = kind;
    payload.memory_cost = memoryCost;
    payload.worker = "rig0-gpu0";
    payload.attempts = 123456;
    payload.hashes_per_second = 1500.5;
    payload.found_at_utc = "2026-08-09T12:00:00Z";
    return payload;
}

Classification classify(FindStatus status, const std::string& reason = "") {
    Classification c;
    c.next_status = status;
    c.reason = reason;
    return c;
}

const std::string kNow = "2026-08-09T12:34:56Z";

// ---------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------

TEST_CASE(append_durable_and_idempotent) {
    TempDb tmp("append_durable_and_idempotent");
    FindJournal journal(tmp.path);

    FoundPayload payload = makePayload("01");
    const std::int64_t id = journal.append(payload);
    CHECK(id > 0);

    // Duplicate key: same id back, still exactly one row.
    payload.worker = "different-worker";  // dupes never overwrite the original capture
    CHECK_EQ(journal.append(payload), id);

    RawDb raw(tmp.path);
    CHECK_EQ(raw.scalarInt("SELECT COUNT(*) FROM finds;"), 1);
    CHECK_EQ(raw.scalarText("SELECT status FROM finds WHERE id=1;").value(), "Pending");
    CHECK_EQ(raw.scalarText("SELECT key FROM finds WHERE id=1;").value(), payload.key);
    CHECK_EQ(raw.scalarText("SELECT worker FROM finds WHERE id=1;").value(), "rig0-gpu0");
    CHECK_EQ(raw.scalarText("SELECT kind FROM finds WHERE id=1;").value(), "XEN11");
    // Durability plumbing really is in effect.
    CHECK_EQ(raw.scalarText("PRAGMA journal_mode;").value(), "wal");
    CHECK_EQ(raw.scalarInt("SELECT version FROM schema_version;"), 1);
}

TEST_CASE(invalid_payload_stored) {
    TempDb tmp("invalid_payload_stored");
    FindJournal journal(tmp.path);

    // Oversize hash_to_verify (server limit 150): stored, but PermanentlyInvalid.
    FoundPayload oversize = makePayload("02");
    oversize.hash_to_verify = std::string(151, 'x');
    const std::int64_t idOversize = journal.append(oversize);
    CHECK(idOversize > 0);

    // Empty account: same handling.
    FoundPayload noAccount = makePayload("03");
    noAccount.account = "";
    const std::int64_t idNoAccount = journal.append(noAccount);
    CHECK(idNoAccount > 0);
    CHECK(idNoAccount != idOversize);

    RawDb raw(tmp.path);
    CHECK_EQ(raw.scalarInt("SELECT COUNT(*) FROM finds;"), 2);
    CHECK_EQ(raw.scalarInt("SELECT COUNT(*) FROM finds "
                           "WHERE status='PermanentlyInvalid' "
                           "AND status_reason IS NOT NULL;"),
             2);
    // Data preserved verbatim (never throw away a find).
    CHECK_EQ(static_cast<std::size_t>(raw.scalarInt(
                 "SELECT LENGTH(hash_to_verify) FROM finds WHERE id=" +
                 std::to_string(idOversize) + ";")),
             oversize.hash_to_verify.size());

    // Invalid rows are never eligible for submission.
    CHECK(journal.fetchEligible(kNow, 10).empty());
}

TEST_CASE(fetch_eligible_backoff_and_ordering) {
    TempDb tmp("fetch_eligible_backoff_and_ordering");
    FindJournal journal(tmp.path);

    const std::int64_t id1 = journal.append(makePayload("11"));
    const std::int64_t id2 = journal.append(makePayload("12"));
    const std::int64_t id3 = journal.append(makePayload("13"));

    // id2 keeps Pending but with a future backoff time.
    journal.recordAttempt(id2, classify(FindStatus::Pending, "transient 503"), 503,
                          "server sick", std::string("2026-08-09T13:00:00Z"), kNow);

    // Before the backoff expires: id2 excluded, order ascending by id.
    auto eligible = journal.fetchEligible("2026-08-09T12:59:59Z", 10);
    CHECK_EQ(eligible.size(), 2u);
    CHECK_EQ(eligible[0].id, id1);
    CHECK_EQ(eligible[1].id, id3);

    // next_attempt_at <= now is inclusive.
    eligible = journal.fetchEligible("2026-08-09T13:00:00Z", 10);
    CHECK_EQ(eligible.size(), 3u);
    CHECK_EQ(eligible[0].id, id1);
    CHECK_EQ(eligible[1].id, id2);
    CHECK_EQ(eligible[2].id, id3);

    // LIMIT respected, oldest first.
    eligible = journal.fetchEligible("2026-08-09T13:00:00Z", 1);
    CHECK_EQ(eligible.size(), 1u);
    CHECK_EQ(eligible[0].id, id1);
}

TEST_CASE(record_attempt_round_trip) {
    TempDb tmp("record_attempt_round_trip");
    FindJournal journal(tmp.path);

    const FoundPayload payload = makePayload("21");
    const std::int64_t id = journal.append(payload);

    // Failed attempt: stays Pending with backoff; every field round-trips.
    journal.recordAttempt(id, classify(FindStatus::Pending, "connect timeout"),
                          std::nullopt, "", std::string("2026-08-09T12:40:00Z"), kNow);
    auto eligible = journal.fetchEligible("2026-08-09T12:40:00Z", 10);
    CHECK_EQ(eligible.size(), 1u);
    const FindRecord& record = eligible[0];
    CHECK_EQ(record.id, id);
    CHECK(record.status == FindStatus::Pending);
    CHECK_EQ(record.status_reason, "connect timeout");
    CHECK_EQ(record.attempt_count, 1);
    CHECK(record.next_attempt_at.has_value());
    CHECK_EQ(record.next_attempt_at.value(), "2026-08-09T12:40:00Z");
    CHECK(record.last_attempt_at.has_value());
    CHECK_EQ(record.last_attempt_at.value(), kNow);
    CHECK(!record.last_http_status.has_value());
    CHECK(!record.confirmed_at.has_value());
    // Immutable payload intact.
    CHECK_EQ(record.payload.key, payload.key);
    CHECK_EQ(record.payload.hash_to_verify, payload.hash_to_verify);
    CHECK_EQ(record.payload.account, payload.account);
    CHECK(record.payload.kind == FindKind::XEN11);
    CHECK_EQ(record.payload.memory_cost, 1727u);
    CHECK_EQ(record.payload.worker, payload.worker);
    CHECK_EQ(record.payload.attempts, payload.attempts);
    CHECK_EQ(record.payload.hashes_per_second, payload.hashes_per_second);
    CHECK_EQ(record.payload.found_at_utc, payload.found_at_utc);

    // Ack: terminal, confirmed_at set to the ack time.
    const std::string ackTime = "2026-08-09T12:45:00Z";
    journal.recordAttempt(id, classify(FindStatus::Acked, "confirmed via get_block"), 200,
                          "OK", std::nullopt, ackTime);
    RawDb raw(tmp.path);
    CHECK_EQ(raw.scalarText("SELECT status FROM finds WHERE id=1;").value(), "Acked");
    CHECK_EQ(raw.scalarText("SELECT confirmed_at FROM finds WHERE id=1;").value(),
             ackTime);
    CHECK_EQ(raw.scalarInt("SELECT attempt_count FROM finds WHERE id=1;"), 2);
    CHECK_EQ(raw.scalarInt("SELECT last_http_status FROM finds WHERE id=1;"), 200);
    CHECK_EQ(raw.scalarText("SELECT last_response FROM finds WHERE id=1;").value(), "OK");
    CHECK(!raw.scalarText("SELECT next_attempt_at FROM finds WHERE id=1;").has_value());

    // A repeated Ack must not move the original confirmation time.
    journal.recordAttempt(id, classify(FindStatus::Acked, "re-ack"), 200, "OK",
                          std::nullopt, "2026-08-09T23:00:00Z");
    CHECK_EQ(raw.scalarText("SELECT confirmed_at FROM finds WHERE id=1;").value(),
             ackTime);

    // Contract guards.
    CHECK_THROWS(journal.recordAttempt(id, classify(FindStatus::Submitting), std::nullopt,
                                       "", std::nullopt, kNow),
                 JournalError);
    CHECK_THROWS(journal.recordAttempt(9999, classify(FindStatus::Pending), std::nullopt,
                                       "", std::nullopt, kNow),
                 JournalError);
}

TEST_CASE(unpark_for_difficulty_boundary) {
    TempDb tmp("unpark_for_difficulty_boundary");
    FindJournal journal(tmp.path);

    const std::int64_t idHigh = journal.append(makePayload("31", FindKind::XEN11, 1500));
    const std::int64_t idLow = journal.append(makePayload("32", FindKind::XEN11, 1400));
    journal.recordAttempt(idHigh, classify(FindStatus::ParkedDifficulty, "difficulty"),
                          401, "difficulty too low", std::string("2026-08-09T13:00:00Z"),
                          kNow);
    journal.recordAttempt(idLow, classify(FindStatus::ParkedDifficulty, "difficulty"),
                          401, "difficulty too low", std::string("2026-08-09T13:00:00Z"),
                          kNow);

    // Boundary: m == current difficulty unparks (server rejects strictly m < current).
    CHECK_EQ(journal.unparkForDifficulty(1500), 1u);

    RawDb raw(tmp.path);
    CHECK_EQ(raw.scalarText("SELECT status FROM finds WHERE id=" +
                            std::to_string(idHigh) + ";")
                 .value(),
             "Pending");
    CHECK_EQ(raw.scalarText("SELECT status FROM finds WHERE id=" +
                            std::to_string(idLow) + ";")
                 .value(),
             "ParkedDifficulty");
    // Backoff cleared: eligible immediately even before the old next_attempt_at.
    auto eligible = journal.fetchEligible(kNow, 10);
    CHECK_EQ(eligible.size(), 1u);
    CHECK_EQ(eligible[0].id, idHigh);
    CHECK(!eligible[0].next_attempt_at.has_value());

    // Second call at a lower difficulty releases the rest; nothing double-unparks.
    CHECK_EQ(journal.unparkForDifficulty(1400), 1u);
    CHECK_EQ(journal.unparkForDifficulty(1400), 0u);
}

TEST_CASE(unpark_xuni_budget_exhaustion) {
    TempDb tmp("unpark_xuni_budget_exhaustion");
    FindJournal journal(tmp.path);
    RawDb raw(tmp.path);

    const std::int64_t id = journal.append(makePayload("41", FindKind::XUNI));
    const auto park = [&] {
        journal.recordAttempt(id, classify(FindStatus::ParkedXuniWindow, "missed window"),
                              401, "XUNI Submitted outside of proper time frame.",
                              std::nullopt, kNow);
    };

    for (int window = 1; window <= 3; ++window) {
        park();
        CHECK_EQ(journal.unparkXuniForWindow(3), 1u);
        CHECK_EQ(raw.scalarInt("SELECT xuni_windows_tried FROM finds WHERE id=1;"),
                 window);
        CHECK_EQ(raw.scalarText("SELECT status FROM finds WHERE id=1;").value(),
                 "Pending");
    }

    // Budget of 3 exhausted: the 4th window kills it.
    park();
    CHECK_EQ(journal.unparkXuniForWindow(3), 0u);
    CHECK_EQ(raw.scalarText("SELECT status FROM finds WHERE id=1;").value(), "Dead");
    CHECK_EQ(raw.scalarText("SELECT status_reason FROM finds WHERE id=1;").value(),
             "xuni window budget exhausted");
    CHECK(journal.fetchEligible(kNow, 10).empty());
}

TEST_CASE(recover_on_startup_counts) {
    TempDb tmp("recover_on_startup_counts");
    {
        FindJournal journal(tmp.path);
        // Two Pending.
        journal.append(makePayload("51"));
        journal.append(makePayload("52"));
        // One of each non-pending state, driven through the public API.
        const auto put = [&](const std::string& suffix, FindStatus status) {
            const std::int64_t id = journal.append(makePayload(suffix));
            journal.recordAttempt(id, classify(status, "test"), std::nullopt, "",
                                  std::nullopt, kNow);
        };
        put("53", FindStatus::AcceptedUnconfirmed);
        put("54", FindStatus::Acked);
        put("55", FindStatus::ParkedDifficulty);
        put("56", FindStatus::ParkedXuniWindow);
        put("57", FindStatus::Quarantined);
        put("58", FindStatus::Dead);
        FoundPayload invalid = makePayload("59");
        invalid.account = "";
        journal.append(invalid);  // PermanentlyInvalid
    }  // close so we can tamper below

    {
        // 'Submitting' can only exist through a bug; inject it raw to prove recovery
        // repairs it. Also stamp a stale backoff that recovery must NOT clear.
        RawDb raw(tmp.path);
        raw.exec("UPDATE finds SET status='Submitting', "
                 "next_attempt_at='2026-08-09T11:00:00Z' WHERE id=1;");
    }

    FindJournal journal(tmp.path);
    const IFindJournal::RecoveryStats stats = journal.recoverOnStartup();
    CHECK_EQ(stats.pending, 2u);  // includes the repaired Submitting row
    CHECK_EQ(stats.accepted_unconfirmed, 1u);
    CHECK_EQ(stats.parked_difficulty, 1u);
    CHECK_EQ(stats.parked_xuni, 1u);
    CHECK_EQ(stats.quarantined, 1u);
    CHECK_EQ(stats.acked, 1u);
    CHECK_EQ(stats.dead, 1u);
    CHECK_EQ(stats.invalid, 1u);

    RawDb raw(tmp.path);
    CHECK_EQ(raw.scalarInt("SELECT COUNT(*) FROM finds WHERE status='Submitting';"), 0);
    // Recovery keeps persisted backoff (SOL-PLAN §6: restarts must not reset backoff).
    CHECK_EQ(raw.scalarText("SELECT next_attempt_at FROM finds WHERE id=1;").value(),
             "2026-08-09T11:00:00Z");

    // counts() mapping: parked aggregates both parked states; strict per-state
    // elsewhere, including the v1.1 slots.
    const IFindJournal::Counts counts = journal.counts();
    CHECK_EQ(counts.pending, 2u);
    CHECK_EQ(counts.parked, 2u);
    CHECK_EQ(counts.quarantined, 1u);
    CHECK_EQ(counts.acked_total, 1u);
    CHECK_EQ(counts.dead_total, 1u);
    CHECK_EQ(counts.accepted_unconfirmed, 1u);
    CHECK_EQ(counts.permanently_invalid, 1u);
}

TEST_CASE(fetch_awaiting_confirmation) {
    TempDb tmp("fetch_awaiting_confirmation");
    FindJournal journal(tmp.path);

    const std::int64_t idPending = journal.append(makePayload("71"));
    const std::int64_t idNoBackoff = journal.append(makePayload("72"));
    const std::int64_t idPastBackoff = journal.append(makePayload("73"));
    const std::int64_t idFutureBackoff = journal.append(makePayload("74"));

    // Three AcceptedUnconfirmed rows: NULL, past, and future next_attempt_at.
    journal.recordAttempt(idNoBackoff,
                          classify(FindStatus::AcceptedUnconfirmed, "lookup down"), 200,
                          "OK", std::nullopt, kNow);
    journal.recordAttempt(idPastBackoff,
                          classify(FindStatus::AcceptedUnconfirmed, "lookup down"), 200,
                          "OK", std::string("2026-08-09T12:00:00Z"), kNow);
    journal.recordAttempt(idFutureBackoff,
                          classify(FindStatus::AcceptedUnconfirmed, "lookup down"), 200,
                          "OK", std::string("2026-08-09T13:00:00Z"), kNow);

    // NULL and past backoffs are fetched oldest-first; the future one is not; Pending
    // rows never leak into the confirmation queue.
    auto awaiting = journal.fetchAwaitingConfirmation(kNow, 10);
    CHECK_EQ(awaiting.size(), 2u);
    CHECK_EQ(awaiting[0].id, idNoBackoff);
    CHECK_EQ(awaiting[1].id, idPastBackoff);
    CHECK(awaiting[0].status == FindStatus::AcceptedUnconfirmed);
    CHECK_EQ(awaiting[0].status_reason, "lookup down");
    CHECK_EQ(awaiting[0].attempt_count, 1);
    CHECK(awaiting[0].last_http_status.has_value());
    CHECK_EQ(awaiting[0].last_http_status.value(), 200);
    CHECK_EQ(awaiting[0].payload.key, makePayload("72").key);  // full hydration

    // Backoff boundary is inclusive, and LIMIT applies.
    awaiting = journal.fetchAwaitingConfirmation("2026-08-09T13:00:00Z", 10);
    CHECK_EQ(awaiting.size(), 3u);
    CHECK_EQ(awaiting[2].id, idFutureBackoff);
    awaiting = journal.fetchAwaitingConfirmation("2026-08-09T13:00:00Z", 1);
    CHECK_EQ(awaiting.size(), 1u);
    CHECK_EQ(awaiting[0].id, idNoBackoff);

    // Conversely fetchEligible sees only the Pending row.
    auto eligible = journal.fetchEligible(kNow, 10);
    CHECK_EQ(eligible.size(), 1u);
    CHECK_EQ(eligible[0].id, idPending);
}

TEST_CASE(get_by_id_hit_and_miss) {
    TempDb tmp("get_by_id_hit_and_miss");
    FindJournal journal(tmp.path);

    const FoundPayload payload = makePayload("81", FindKind::XUNI, 1600);
    const std::int64_t id = journal.append(payload);
    const std::string ackTime = "2026-08-09T12:45:00Z";
    journal.recordAttempt(id, classify(FindStatus::Acked, "confirmed"), 200, "OK",
                          std::nullopt, ackTime);

    // Hit: full hydration, including a terminal state fetchEligible would never expose.
    const auto found = journal.getById(id);
    CHECK(found.has_value());
    CHECK_EQ(found->id, id);
    CHECK(found->status == FindStatus::Acked);
    CHECK_EQ(found->status_reason, "confirmed");
    CHECK_EQ(found->attempt_count, 1);
    CHECK(found->confirmed_at.has_value());
    CHECK_EQ(found->confirmed_at.value(), ackTime);
    CHECK(found->last_attempt_at.has_value());
    CHECK_EQ(found->last_attempt_at.value(), ackTime);
    CHECK_EQ(found->last_response, "OK");
    CHECK_EQ(found->payload.key, payload.key);
    CHECK_EQ(found->payload.hash_to_verify, payload.hash_to_verify);
    CHECK_EQ(found->payload.account, payload.account);
    CHECK(found->payload.kind == FindKind::XUNI);
    CHECK_EQ(found->payload.memory_cost, 1600u);
    CHECK_EQ(found->payload.found_at_utc, payload.found_at_utc);

    // Miss: nullopt, no throw.
    CHECK(!journal.getById(9999).has_value());
}

TEST_CASE(difficulty_seen_round_trip) {
    TempDb tmp("difficulty_seen_round_trip");
    FindJournal journal(tmp.path);

    CHECK(!journal.lastKnownDifficulty().has_value());

    journal.recordDifficulty(1727, "2026-08-09T12:00:00Z");
    journal.recordDifficulty(2727, "2026-08-09T12:05:00Z");
    journal.recordDifficulty(1587, "2026-08-09T12:10:00Z");

    const auto last = journal.lastKnownDifficulty();
    CHECK(last.has_value());
    CHECK_EQ(last.value(), 1587u);

    RawDb raw(tmp.path);
    CHECK_EQ(raw.scalarInt("SELECT COUNT(*) FROM difficulty_seen;"), 3);
    CHECK_EQ(raw.scalarText("SELECT at FROM difficulty_seen ORDER BY rowid LIMIT 1;")
                 .value(),
             "2026-08-09T12:00:00Z");
    CHECK_EQ(raw.scalarInt(
                 "SELECT value FROM difficulty_seen ORDER BY rowid LIMIT 1;"),
             1727);
}

TEST_CASE(reopen_persistence) {
    TempDb tmp("reopen_persistence");
    const FoundPayload keep = makePayload("61");
    std::int64_t keptId = -1;

    {
        FindJournal journal(tmp.path);
        keptId = journal.append(keep);
        const std::int64_t ackedId = journal.append(makePayload("62"));
        journal.recordAttempt(ackedId, classify(FindStatus::Acked, "confirmed"), 200,
                              "OK", std::nullopt, kNow);
        journal.recordDifficulty(1727, "2026-08-09T12:00:00Z");
    }  // destructor closes the connection

    FindJournal reopened(tmp.path);
    // Full payload intact across close/reopen.
    const auto eligible = reopened.fetchEligible(kNow, 10);
    CHECK_EQ(eligible.size(), 1u);
    CHECK_EQ(eligible[0].id, keptId);
    CHECK_EQ(eligible[0].payload.key, keep.key);
    CHECK_EQ(eligible[0].payload.hash_to_verify, keep.hash_to_verify);
    CHECK_EQ(eligible[0].payload.account, keep.account);
    CHECK_EQ(eligible[0].payload.found_at_utc, keep.found_at_utc);

    const IFindJournal::Counts counts = reopened.counts();
    CHECK_EQ(counts.pending, 1u);
    CHECK_EQ(counts.acked_total, 1u);

    const auto difficulty = reopened.lastKnownDifficulty();
    CHECK(difficulty.has_value());
    CHECK_EQ(difficulty.value(), 1727u);

    // Appending the same key after reopen is still idempotent.
    CHECK_EQ(reopened.append(keep), keptId);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Runner: no arguments = all tests; otherwise each argument names one test (CTest mode).
// ---------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::vector<std::string> selected;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) selected.emplace_back(argv[i]);
    } else {
        for (const auto& entry : registry()) selected.push_back(entry.first);
    }

    int failures = 0;
    for (const auto& name : selected) {
        const auto it = registry().find(name);
        if (it == registry().end()) {
            std::cerr << "[MISSING] unknown test: " << name << "\n";
            ++failures;
            continue;
        }
        try {
            it->second();
            std::cout << "[ PASS ] " << name << "\n";
        } catch (const std::exception& error) {
            std::cerr << "[ FAIL ] " << name << "\n         " << error.what() << "\n";
            ++failures;
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all " << selected.size() << " test(s) passed\n";
    return 0;
}
