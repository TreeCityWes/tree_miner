// FallbackSinkTests.cpp — standalone unit tests for treeminer::FallbackSink, the
// last-resort JSONL sink that catches finds when FindJournal::append() throws
// (audit finding A against the journal-first invariant, PLAN.md §3.1).
//
// No GPU, no network. Import tests run against the REAL FindJournal on a fresh SQLite
// file in the system temp directory — the point of this component is what survives into
// the journal, so a mock journal would test the wrong thing. Each case is registered as
// one CTest case (see CMakeLists.txt); running the binary with no arguments executes
// every test. Framework and helper conventions mirror FindJournalTests.cpp deliberately.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "journal/FallbackSink.h"
#include "journal/FindJournal.h"

namespace {

using treeminer::FallbackSink;
using treeminer::FindJournal;
using treeminer::FindKind;
using treeminer::FindRecord;
using treeminer::FindStatus;
using treeminer::FoundPayload;

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
                ("treeminer_fallback_test_" + testName + ".db"))
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

// Fresh sink path per test; removes both the sink and its ".imported" archive on
// construction and destruction, so a previous run's archive can never make a later run
// pass by accident.
struct TempSink {
    explicit TempSink(const std::string& testName) {
        path = (std::filesystem::temp_directory_path() /
                ("treeminer_fallback_sink_" + testName + ".jsonl"))
                   .string();
        cleanup();
    }
    ~TempSink() { cleanup(); }

    void cleanup() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + ".imported", ec);
    }

    bool exists() const {
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }
    bool archiveExists() const {
        std::error_code ec;
        return std::filesystem::exists(path + ".imported", ec);
    }

    std::vector<std::string> readLines() const {
        std::vector<std::string> lines;
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        return lines;
    }

    void writeLines(const std::vector<std::string>& lines) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) out << line << "\n";
    }

    std::string path;
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

const std::string kNow = "2026-08-09T12:34:56Z";

// Every journaled Pending record, keyed by payload.key. The sink's whole contract is
// "the find reaches the journal intact", so tests assert on hydrated records rather than
// on the sink file's bytes.
std::map<std::string, FindRecord> journaledByKey(FindJournal& journal) {
    std::map<std::string, FindRecord> byKey;
    for (const auto& record : journal.fetchEligible(kNow, 1000)) {
        byKey.emplace(record.payload.key, record);
    }
    return byKey;
}

// Field-by-field equality of the payload as it came back out of the journal. Written out
// long-hand rather than as operator== so a failure names the field that drifted.
void checkPayloadsEqual(const FoundPayload& actual, const FoundPayload& expected) {
    CHECK_EQ(actual.key, expected.key);
    CHECK_EQ(actual.hash_to_verify, expected.hash_to_verify);
    CHECK_EQ(actual.account, expected.account);
    CHECK(actual.kind == expected.kind);
    CHECK_EQ(actual.memory_cost, expected.memory_cost);
    CHECK_EQ(actual.worker, expected.worker);
    CHECK_EQ(actual.attempts, expected.attempts);
    CHECK_EQ(actual.hashes_per_second, expected.hashes_per_second);
    CHECK_EQ(actual.found_at_utc, expected.found_at_utc);
}

// ---------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------

// The core path: SQLite was broken, three finds fell into the sink, the next boot drains
// them back with nothing lost or altered.
TEST_CASE(append_and_import_round_trip) {
    TempSink sink("round_trip");
    TempDb tmp("round_trip");

    FoundPayload a = makePayload("01", FindKind::XEN11, 1727);
    FoundPayload b = makePayload("02", FindKind::XUNI, 1900);
    b.worker = "rig1-gpu3";
    b.attempts = 987654321ull;
    b.hashes_per_second = 0.125;  // exactly representable; %.17g must not perturb it
    b.found_at_utc = "2026-08-09T12:05:01Z";
    FoundPayload c = makePayload("03", FindKind::XEN11, 2048);
    c.hashes_per_second = 1234.56789;  // NOT exactly representable: the real round-trip test

    FallbackSink fallback(sink.path);
    CHECK(fallback.append(a));
    CHECK(fallback.append(b));
    CHECK(fallback.append(c));
    CHECK_EQ(sink.readLines().size(), 3u);

    FindJournal journal(tmp.path);
    const FallbackSink::ImportStats stats = FallbackSink::importInto(journal, sink.path);
    CHECK_EQ(stats.imported, 3u);
    CHECK_EQ(stats.malformed, 0u);
    CHECK(stats.file_present);

    const auto byKey = journaledByKey(journal);
    CHECK_EQ(byKey.size(), 3u);
    for (const FoundPayload& expected : {a, b, c}) {
        const auto it = byKey.find(expected.key);
        CHECK(it != byKey.end());
        CHECK(it->second.status == FindStatus::Pending);
        checkPayloadsEqual(it->second.payload, expected);
    }

    // Clean pass: the sink is archived, not deleted (evidence), and the next boot starts
    // from an empty sink.
    CHECK(!sink.exists());
    CHECK(sink.archiveExists());
}

// Re-import must be free: the sink may hold a record that SQLite actually did commit
// (append threw after the INSERT), or that a previous boot already imported before dying
// ahead of the rename. Both collapse onto the journal's UNIQUE key.
TEST_CASE(import_is_idempotent_by_key) {
    TempSink sink("idempotent");
    TempDb tmp("idempotent");

    const FoundPayload payload = makePayload("07", FindKind::XUNI, 1800);

    FallbackSink fallback(sink.path);
    CHECK(fallback.append(payload));
    CHECK(fallback.append(payload));  // same key twice in the sink

    FindJournal journal(tmp.path);
    const std::int64_t directId = journal.append(payload);  // and once directly
    CHECK(directId > 0);

    const FallbackSink::ImportStats stats = FallbackSink::importInto(journal, sink.path);
    CHECK_EQ(stats.imported, 2u);  // both lines were handed over...
    CHECK_EQ(stats.malformed, 0u);

    const auto byKey = journaledByKey(journal);
    CHECK_EQ(byKey.size(), 1u);  // ...and both deduped onto the original row
    CHECK_EQ(byKey.at(payload.key).id, directId);
    checkPayloadsEqual(byKey.at(payload.key).payload, payload);
}

// One damaged line must never strand the good records behind it — that would recreate the
// exact drop this component exists to prevent. A torn tail from a crash mid-write is the
// realistic source of the truncated line.
TEST_CASE(malformed_lines_never_block_recovery) {
    TempSink sink("malformed");
    TempDb tmp("malformed");

    const FoundPayload first = makePayload("11", FindKind::XEN11, 1727);
    const FoundPayload second = makePayload("12", FindKind::XUNI, 1900);

    // Generate the two good lines through the real serializer, then hand-assemble a file
    // with damage between them, so the test cannot drift from the sink's actual format.
    std::vector<std::string> goodLines;
    {
        TempSink scratch("malformed_scratch");
        FallbackSink generator(scratch.path);
        CHECK(generator.append(first));
        CHECK(generator.append(second));
        goodLines = scratch.readLines();
        CHECK_EQ(goodLines.size(), 2u);
    }

    std::string truncated = goodLines[0];
    truncated.resize(truncated.size() / 2);  // cut mid-object, likely mid-string

    sink.writeLines({goodLines[0], "not json", truncated, goodLines[1]});

    FindJournal journal(tmp.path);
    const FallbackSink::ImportStats stats = FallbackSink::importInto(journal, sink.path);
    CHECK_EQ(stats.imported, 2u);
    CHECK_EQ(stats.malformed, 2u);
    CHECK(stats.file_present);

    const auto byKey = journaledByKey(journal);
    CHECK_EQ(byKey.size(), 2u);
    checkPayloadsEqual(byKey.at(first.key).payload, first);
    checkPayloadsEqual(byKey.at(second.key).payload, second);

    // Malformed lines are skipped, not fatal: the pass still counts as clean and archives.
    CHECK(!sink.exists());
    CHECK(sink.archiveExists());
}

// The overwhelmingly common boot: SQLite never failed, so no sink was ever created.
// Import must be a silent no-op that neither errors nor creates the file.
TEST_CASE(missing_file_is_clean_noop) {
    TempSink sink("missing");
    TempDb tmp("missing");
    CHECK(!sink.exists());

    FindJournal journal(tmp.path);
    const FallbackSink::ImportStats stats = FallbackSink::importInto(journal, sink.path);
    CHECK_EQ(stats.imported, 0u);
    CHECK_EQ(stats.malformed, 0u);
    CHECK(!stats.file_present);

    CHECK(!sink.exists());
    CHECK(!sink.archiveExists());
    CHECK_EQ(journal.counts().pending, 0u);
}

// JSONL is a line-oriented format holding fields the miner does not sanitize. Quotes,
// backslashes, embedded newlines and non-ASCII bytes must survive escape/unescape
// byte-exactly, or a "recovered" find would be submitted with a corrupted payload.
TEST_CASE(append_survives_weird_content) {
    TempSink sink("weird");
    TempDb tmp("weird");

    FoundPayload payload = makePayload("21", FindKind::XUNI, 4096);
    // hash_to_verify stays under the journal's 150-char server limit so the record lands
    // Pending rather than PermanentlyInvalid; the nastiness goes in the free-text fields.
    payload.hash_to_verify = "$argon2id$v=19$m=4096,t=1,p=1$c2FsdA$\"quoted\\backslash\"";
    payload.worker = "rig\"0\\gpu\nline2\ttab\x01ctrl-\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac";
    payload.found_at_utc = "2026-08-09T12:00:00Z\r\nspliced";
    payload.attempts = 18446744073709551615ull;  // u64 max: no silent narrowing
    payload.hashes_per_second = 0.1 + 0.2;       // 0.30000000000000004; %.17g or bust

    FallbackSink fallback(sink.path);
    CHECK(fallback.append(payload));

    // Embedded newlines must have been escaped, not written raw: one find, one line.
    CHECK_EQ(sink.readLines().size(), 1u);

    FindJournal journal(tmp.path);
    const FallbackSink::ImportStats stats = FallbackSink::importInto(journal, sink.path);
    CHECK_EQ(stats.imported, 1u);
    CHECK_EQ(stats.malformed, 0u);

    const auto byKey = journaledByKey(journal);
    CHECK_EQ(byKey.size(), 1u);
    checkPayloadsEqual(byKey.at(payload.key).payload, payload);
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
