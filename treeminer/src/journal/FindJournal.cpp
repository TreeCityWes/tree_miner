// FindJournal.cpp — SQLite implementation of the durable find journal.
// Schema: PLAN.md §3.1 extended by SOL-PLAN.md §4; contract: src/treeminer/IFindJournal.h.

#include "journal/FindJournal.h"

#include <sqlite3.h>

#include <cstddef>
#include <utility>

namespace treeminer {

// NOTE: to_string(FindStatus)/to_string(FindKind) are defined ONLY in
// src/treeminer/Types.cpp (integration-lead-owned, contract v1.1); this component links
// against that TU and must not define them. The returned strings ARE the persisted DB
// values and match the Types.h enumerator names exactly.

namespace {

// Server-side limit on hash_to_verify (oversize payloads are rejected outright).
constexpr std::size_t kMaxHashToVerifyLength = 150;
constexpr int kSupportedSchemaVersion = 1;
constexpr int kBusyTimeoutMs = 5000;

FindStatus statusFromString(const std::string& s) {
    if (s == "Pending")             return FindStatus::Pending;
    if (s == "Submitting")          return FindStatus::Submitting;
    if (s == "AcceptedUnconfirmed") return FindStatus::AcceptedUnconfirmed;
    if (s == "Acked")               return FindStatus::Acked;
    if (s == "ParkedDifficulty")    return FindStatus::ParkedDifficulty;
    if (s == "ParkedXuniWindow")    return FindStatus::ParkedXuniWindow;
    if (s == "Quarantined")         return FindStatus::Quarantined;
    if (s == "Dead")                return FindStatus::Dead;
    if (s == "PermanentlyInvalid")  return FindStatus::PermanentlyInvalid;
    throw JournalError("journal: unknown status string in database: '" + s + "'");
}

FindKind kindFromString(const std::string& s) {
    if (s == "XEN11") return FindKind::XEN11;
    if (s == "XUNI")  return FindKind::XUNI;
    throw JournalError("journal: unknown kind string in database: '" + s + "'");
}

std::string sqliteMessage(sqlite3* db, int rc, const char* context) {
    std::string message = "journal: ";
    message += context;
    message += ": ";
    message += (db != nullptr) ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    message += " (rc=" + std::to_string(rc) + ")";
    return message;
}

// RAII wrapper around sqlite3_stmt*. Finalizes on every path; throws JournalError with
// the connection's error text on any bind/step failure.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            throw JournalError(sqliteMessage(db_, rc, "prepare"));
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bindText(int index, const std::string& value) {
        check(sqlite3_bind_text(stmt_, index, value.c_str(),
                                static_cast<int>(value.size()), SQLITE_TRANSIENT),
              "bind text");
    }
    // Empty/absent values are stored as NULL for nullable columns.
    void bindTextOrNull(int index, const std::string& value) {
        if (value.empty()) {
            bindNull(index);
        } else {
            bindText(index, value);
        }
    }
    void bindOptionalText(int index, const std::optional<std::string>& value) {
        if (value.has_value()) {
            bindText(index, *value);
        } else {
            bindNull(index);
        }
    }
    void bindInt64(int index, std::int64_t value) {
        check(sqlite3_bind_int64(stmt_, index, value), "bind int64");
    }
    void bindOptionalInt(int index, const std::optional<int>& value) {
        if (value.has_value()) {
            bindInt64(index, *value);
        } else {
            bindNull(index);
        }
    }
    void bindDouble(int index, double value) {
        check(sqlite3_bind_double(stmt_, index, value), "bind double");
    }
    void bindNull(int index) { check(sqlite3_bind_null(stmt_, index), "bind null"); }

    // True while a row is available; false once done.
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw JournalError(sqliteMessage(db_, rc, "step"));
    }

    bool columnIsNull(int column) const {
        return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
    }
    std::int64_t columnInt64(int column) const { return sqlite3_column_int64(stmt_, column); }
    double columnDouble(int column) const { return sqlite3_column_double(stmt_, column); }
    std::string columnText(int column) const {
        const unsigned char* text = sqlite3_column_text(stmt_, column);
        return text != nullptr ? reinterpret_cast<const char*>(text) : std::string();
    }
    std::optional<std::string> columnOptionalText(int column) const {
        if (columnIsNull(column)) return std::nullopt;
        return columnText(column);
    }
    std::optional<int> columnOptionalInt(int column) const {
        if (columnIsNull(column)) return std::nullopt;
        return static_cast<int>(columnInt64(column));
    }

private:
    void check(int rc, const char* context) {
        if (rc != SQLITE_OK) {
            throw JournalError(sqliteMessage(db_, rc, context));
        }
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// RAII transaction: rolls back on scope exit unless committed. BEGIN IMMEDIATE takes the
// write lock up front so a busy database fails fast (bounded by busy_timeout) instead of
// failing mid-transaction.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        char* errorText = nullptr;
        const int rc = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &errorText);
        if (rc != SQLITE_OK) {
            std::string message = sqliteMessage(db_, rc, "BEGIN IMMEDIATE");
            sqlite3_free(errorText);
            throw JournalError(message);
        }
        active_ = true;
    }
    ~Transaction() {
        if (active_) {
            // Best effort; never throw from a destructor.
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // Durable on return: with synchronous=FULL the COMMIT fsyncs the WAL.
    void commit() {
        char* errorText = nullptr;
        const int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errorText);
        if (rc != SQLITE_OK) {
            std::string message = sqliteMessage(db_, rc, "COMMIT");
            sqlite3_free(errorText);
            throw JournalError(message);
        }
        active_ = false;
    }

private:
    sqlite3* db_ = nullptr;
    bool active_ = false;
};

constexpr const char* kRecordColumns =
    "id, key, hash_to_verify, account, kind, m, worker, attempts, hashes_per_second, "
    "found_at, status, status_reason, attempt_count, next_attempt_at, last_attempt_at, "
    "last_http_status, last_response, confirmed_at, xuni_windows_tried";

FindRecord rowToRecord(const Statement& row) {
    FindRecord record;
    record.id = row.columnInt64(0);
    record.payload.key = row.columnText(1);
    record.payload.hash_to_verify = row.columnText(2);
    record.payload.account = row.columnText(3);
    record.payload.kind = kindFromString(row.columnText(4));
    record.payload.memory_cost = static_cast<std::uint32_t>(row.columnInt64(5));
    record.payload.worker = row.columnText(6);
    record.payload.attempts = static_cast<std::uint64_t>(row.columnInt64(7));
    record.payload.hashes_per_second = row.columnDouble(8);
    record.payload.found_at_utc = row.columnText(9);
    record.status = statusFromString(row.columnText(10));
    record.status_reason = row.columnText(11);
    record.attempt_count = static_cast<std::int32_t>(row.columnInt64(12));
    record.next_attempt_at = row.columnOptionalText(13);
    record.last_attempt_at = row.columnOptionalText(14);
    record.last_http_status = row.columnOptionalInt(15);
    record.last_response = row.columnText(16);
    record.confirmed_at = row.columnOptionalText(17);
    record.xuni_windows_tried = static_cast<std::int32_t>(row.columnInt64(18));
    return record;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Construction / schema
// ---------------------------------------------------------------------------------------

FindJournal::FindJournal(const std::string& dbPath) {
    const int rc = sqlite3_open_v2(dbPath.c_str(), &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = sqliteMessage(db_, rc, ("open '" + dbPath + "'").c_str());
        sqlite3_close_v2(db_);  // sqlite allocates a handle even on open failure
        db_ = nullptr;
        throw JournalError(message);
    }
    try {
        applyPragmas();
        ensureSchema();
    } catch (...) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        throw;
    }
}

FindJournal::~FindJournal() {
    if (db_ != nullptr) {
        // Orderly-shutdown checkpoint (best effort; durability never depends on it).
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
        sqlite3_close_v2(db_);
    }
}

void FindJournal::execOrThrow(const char* sql, const char* context) {
    char* errorText = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errorText);
    if (rc != SQLITE_OK) {
        std::string message = sqliteMessage(db_, rc, context);
        sqlite3_free(errorText);
        throw JournalError(message);
    }
}

void FindJournal::applyPragmas() {
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);

    // journal_mode returns the mode actually in effect; anything but WAL voids the
    // durability model, so treat a fallback (e.g. unsupported filesystem) as fatal.
    {
        Statement walQuery(db_, "PRAGMA journal_mode=WAL;");
        if (!walQuery.step() || walQuery.columnText(0) != "wal") {
            throw JournalError("journal: could not enable WAL journal mode at this path");
        }
    }
    execOrThrow("PRAGMA synchronous=FULL;", "PRAGMA synchronous");
    execOrThrow("PRAGMA foreign_keys=ON;", "PRAGMA foreign_keys");
}

void FindJournal::ensureSchema() {
    Transaction tx(db_);
    execOrThrow(
        "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);",
        "create schema_version");
    execOrThrow(
        "CREATE TABLE IF NOT EXISTS finds ("
        "  id                 INTEGER PRIMARY KEY,"
        "  key                TEXT NOT NULL UNIQUE,"
        "  hash_to_verify     TEXT NOT NULL,"
        "  account            TEXT NOT NULL,"
        "  kind               TEXT NOT NULL CHECK(kind IN ('XEN11','XUNI')),"
        "  m                  INTEGER NOT NULL,"
        "  worker             TEXT,"
        "  attempts           INTEGER,"
        "  hashes_per_second  REAL,"
        "  found_at           TEXT NOT NULL,"
        "  status             TEXT NOT NULL,"
        "  status_reason      TEXT,"
        "  attempt_count      INTEGER NOT NULL DEFAULT 0,"
        "  next_attempt_at    TEXT,"
        "  last_attempt_at    TEXT,"
        "  last_http_status   INTEGER,"
        "  last_response      TEXT,"
        "  confirmed_at       TEXT,"
        "  xuni_windows_tried INTEGER NOT NULL DEFAULT 0"
        ");",
        "create finds");
    execOrThrow(
        "CREATE INDEX IF NOT EXISTS idx_finds_ready"
        "  ON finds(status, next_attempt_at, kind, id);",
        "create idx_finds_ready");
    execOrThrow(
        "CREATE TABLE IF NOT EXISTS difficulty_seen (at TEXT, value INTEGER);",
        "create difficulty_seen");

    Statement versionQuery(db_, "SELECT MAX(version) FROM schema_version;");
    versionQuery.step();
    if (versionQuery.columnIsNull(0)) {
        Statement insert(db_, "INSERT INTO schema_version(version) VALUES (?1);");
        insert.bindInt64(1, kSupportedSchemaVersion);
        insert.step();
    } else if (versionQuery.columnInt64(0) != kSupportedSchemaVersion) {
        throw JournalError("journal: unsupported schema_version " +
                           std::to_string(versionQuery.columnInt64(0)) + " (supported: " +
                           std::to_string(kSupportedSchemaVersion) + ")");
    }
    tx.commit();
}

// ---------------------------------------------------------------------------------------
// IFindJournal
// ---------------------------------------------------------------------------------------

std::int64_t FindJournal::append(const FoundPayload& payload) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Never throw away a find: payloads the server is known to reject are journaled
    // anyway, just directly in PermanentlyInvalid with the reason recorded.
    FindStatus status = FindStatus::Pending;
    std::string reason;
    if (payload.hash_to_verify.size() > kMaxHashToVerifyLength) {
        status = FindStatus::PermanentlyInvalid;
        reason = "hash_to_verify length " + std::to_string(payload.hash_to_verify.size()) +
                 " exceeds server limit of " + std::to_string(kMaxHashToVerifyLength);
    } else if (payload.account.empty()) {
        status = FindStatus::PermanentlyInvalid;
        reason = "account is empty";
    }

    Transaction tx(db_);
    {
        Statement insert(db_,
            "INSERT INTO finds (key, hash_to_verify, account, kind, m, worker, attempts,"
            "                   hashes_per_second, found_at, status, status_reason)"
            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"
            " ON CONFLICT(key) DO NOTHING;");
        insert.bindText(1, payload.key);
        insert.bindText(2, payload.hash_to_verify);
        insert.bindText(3, payload.account);
        insert.bindText(4, to_string(payload.kind));
        insert.bindInt64(5, static_cast<std::int64_t>(payload.memory_cost));
        insert.bindTextOrNull(6, payload.worker);
        insert.bindInt64(7, static_cast<std::int64_t>(payload.attempts));
        insert.bindDouble(8, payload.hashes_per_second);
        insert.bindText(9, payload.found_at_utc);
        insert.bindText(10, to_string(status));
        insert.bindTextOrNull(11, reason);
        insert.step();
    }

    std::int64_t id = -1;
    if (sqlite3_changes(db_) == 0) {
        // Duplicate local capture: idempotently return the existing row's id.
        Statement select(db_, "SELECT id FROM finds WHERE key = ?1;");
        select.bindText(1, payload.key);
        if (!select.step()) {
            throw JournalError("journal: append hit UNIQUE conflict but key not found");
        }
        id = select.columnInt64(0);
    } else {
        id = sqlite3_last_insert_rowid(db_);
    }
    tx.commit();  // durable on return (WAL + synchronous=FULL)
    return id;
}

std::vector<FindRecord> FindJournal::fetchByStatusLocked(const char* statusLiteral,
                                                         const std::string& now_utc,
                                                         std::size_t limit,
                                                         const char* kindLiteral) {
    std::string sql = "SELECT ";
    sql += kRecordColumns;
    sql += " FROM finds"
           " WHERE status = '";
    sql += statusLiteral;  // trusted compile-time literal, see header
    sql += "'";
    if (kindLiteral != nullptr) {
        sql += " AND kind = '";
        sql += kindLiteral;  // trusted compile-time literal (to_string(FindKind))
        sql += "'";
    }
    sql += "   AND (next_attempt_at IS NULL OR next_attempt_at <= ?1)"
           " ORDER BY id ASC LIMIT ?2;";
    Statement select(db_, sql.c_str());
    select.bindText(1, now_utc);
    select.bindInt64(2, static_cast<std::int64_t>(limit));

    std::vector<FindRecord> records;
    while (select.step()) {
        records.push_back(rowToRecord(select));
    }
    return records;
}

std::vector<FindRecord> FindJournal::fetchEligible(const std::string& now_utc,
                                                   std::size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    return fetchByStatusLocked("Pending", now_utc, limit);
}

std::vector<FindRecord> FindJournal::fetchEligibleOfKind(FindKind kind,
                                                         const std::string& now_utc,
                                                         std::size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    // to_string(FindKind) yields the exact literal stored in the kind column ('XEN11'/'XUNI')
    // and is a fixed compile-time string, never caller input.
    return fetchByStatusLocked("Pending", now_utc, limit, to_string(kind));
}

std::vector<FindRecord> FindJournal::fetchAwaitingConfirmation(const std::string& now_utc,
                                                               std::size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    return fetchByStatusLocked("AcceptedUnconfirmed", now_utc, limit);
}

std::optional<FindRecord> FindJournal::getById(std::int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql = "SELECT ";
    sql += kRecordColumns;
    sql += " FROM finds WHERE id = ?1;";
    Statement select(db_, sql.c_str());
    select.bindInt64(1, id);
    if (!select.step()) {
        return std::nullopt;
    }
    return rowToRecord(select);
}

void FindJournal::recordAttempt(std::int64_t id, const Classification& c,
                                std::optional<int> http_status,
                                const std::string& response_body,
                                const std::optional<std::string>& next_attempt_at,
                                const std::string& now_utc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (c.next_status == FindStatus::Submitting) {
        throw JournalError(
            "journal: recordAttempt with status Submitting — that state is in-process "
            "only and must never be persisted");
    }

    Statement update(db_,
        "UPDATE finds SET"
        "  status = ?1,"
        "  status_reason = ?2,"
        "  attempt_count = attempt_count + 1,"
        "  last_attempt_at = ?3,"
        "  last_http_status = ?4,"
        "  last_response = ?5,"
        "  next_attempt_at = ?6,"
        "  confirmed_at = CASE WHEN ?1 = 'Acked'"
        "                      THEN COALESCE(confirmed_at, ?3) ELSE confirmed_at END"
        " WHERE id = ?7;");
    update.bindText(1, to_string(c.next_status));
    update.bindTextOrNull(2, c.reason);
    update.bindText(3, now_utc);
    update.bindOptionalInt(4, http_status);
    update.bindText(5, response_body);
    update.bindOptionalText(6, next_attempt_at);
    update.bindInt64(7, id);
    update.step();
    if (sqlite3_changes(db_) == 0) {
        throw JournalError("journal: recordAttempt for unknown find id " +
                           std::to_string(id));
    }
}

std::size_t FindJournal::unparkForDifficulty(std::uint32_t current_difficulty) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Server accepts when submitted m >= current difficulty (rejection is strictly '<'),
    // so the boundary m == current_difficulty un-parks.
    Statement update(db_,
        "UPDATE finds SET status = 'Pending', next_attempt_at = NULL"
        " WHERE status = 'ParkedDifficulty' AND m >= ?1;");
    update.bindInt64(1, static_cast<std::int64_t>(current_difficulty));
    update.step();
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

std::size_t FindJournal::unparkXuniForWindow(std::int32_t max_windows) {
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction tx(db_);
    {
        Statement exhaust(db_,
            "UPDATE finds SET status = 'Dead',"
            "  status_reason = 'xuni window budget exhausted',"
            "  next_attempt_at = NULL"
            " WHERE status = 'ParkedXuniWindow' AND xuni_windows_tried >= ?1;");
        exhaust.bindInt64(1, max_windows);
        exhaust.step();
    }
    std::size_t unparked = 0;
    {
        Statement unpark(db_,
            "UPDATE finds SET status = 'Pending',"
            "  xuni_windows_tried = xuni_windows_tried + 1,"
            "  next_attempt_at = NULL"
            " WHERE status = 'ParkedXuniWindow';");  // survivors all have budget left
        unpark.step();
        unparked = static_cast<std::size_t>(sqlite3_changes(db_));
    }
    tx.commit();
    return unparked;
}

IFindJournal::RecoveryStats FindJournal::recoverOnStartup() {
    std::lock_guard<std::mutex> lock(mutex_);
    Transaction tx(db_);

    // 'Submitting' is never persisted, so this should always update zero rows; it exists
    // so a bug elsewhere can never strand a find. Persisted next_attempt_at is kept:
    // restarts must not reset backoff (SOL-PLAN §6).
    execOrThrow("UPDATE finds SET status = 'Pending' WHERE status = 'Submitting';",
                "recover Submitting leftovers");

    RecoveryStats stats;
    Statement select(db_, "SELECT status, COUNT(*) FROM finds GROUP BY status;");
    while (select.step()) {
        const auto count = static_cast<std::size_t>(select.columnInt64(1));
        switch (statusFromString(select.columnText(0))) {
            case FindStatus::Pending:             stats.pending += count; break;
            case FindStatus::AcceptedUnconfirmed: stats.accepted_unconfirmed += count; break;
            case FindStatus::ParkedDifficulty:    stats.parked_difficulty += count; break;
            case FindStatus::ParkedXuniWindow:    stats.parked_xuni += count; break;
            case FindStatus::Quarantined:         stats.quarantined += count; break;
            case FindStatus::Acked:               stats.acked += count; break;
            case FindStatus::Dead:                stats.dead += count; break;
            case FindStatus::PermanentlyInvalid:  stats.invalid += count; break;
            case FindStatus::Submitting:          break;  // unreachable: reset above
        }
    }
    tx.commit();
    return stats;
}

void FindJournal::recordDifficulty(std::uint32_t difficulty, const std::string& at_utc) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement insert(db_, "INSERT INTO difficulty_seen (at, value) VALUES (?1, ?2);");
    insert.bindText(1, at_utc);
    insert.bindInt64(2, static_cast<std::int64_t>(difficulty));
    insert.step();
}

std::optional<std::uint32_t> FindJournal::lastKnownDifficulty() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Most recently recorded observation (insertion order), independent of timestamp
    // formatting — the journal never interprets caller-provided time strings.
    Statement select(db_,
        "SELECT value FROM difficulty_seen ORDER BY rowid DESC LIMIT 1;");
    if (!select.step() || select.columnIsNull(0)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(select.columnInt64(0));
}

IFindJournal::Counts FindJournal::counts() {
    std::lock_guard<std::mutex> lock(mutex_);
    Counts result;
    Statement select(db_, "SELECT status, COUNT(*) FROM finds GROUP BY status;");
    while (select.step()) {
        const auto count = static_cast<std::size_t>(select.columnInt64(1));
        switch (statusFromString(select.columnText(0))) {
            case FindStatus::Pending:             result.pending += count; break;
            case FindStatus::ParkedDifficulty:
                result.parked += count;
                result.parked_difficulty += count;
                break;
            case FindStatus::ParkedXuniWindow:
                result.parked += count;
                result.parked_xuni += count;
                break;
            case FindStatus::Quarantined:         result.quarantined += count; break;
            case FindStatus::Acked:               result.acked_total += count; break;
            case FindStatus::Dead:                result.dead_total += count; break;
            case FindStatus::AcceptedUnconfirmed: result.accepted_unconfirmed += count; break;
            case FindStatus::PermanentlyInvalid:  result.permanently_invalid += count; break;
            case FindStatus::Submitting:          break;  // never persisted
        }
    }
    return result;
}

}  // namespace treeminer
