#pragma once
// FallbackSink.h — last-resort, append-only plaintext capture for finds that the SQLite
// journal refused to accept. Closes audit finding A against the journal-first invariant
// (PLAN.md §3.1: "INSERT + fsync completes before the first submission attempt").
//
// WHY THIS EXISTS
// ---------------
// FindJournal::append() throws JournalError on any SQLite failure. Before this sink, the
// miner's submit callback caught that, logged it, and DROPPED the find — the precise
// failure class this project was built to eliminate. A dropped find is unrecoverable
// value: the Argon2 key that produced it is not reproducible on demand.
//
// WHAT IT ACTUALLY COVERS (and what it does not — read this honestly)
// ------------------------------------------------------------------
// This sink writes to a file on the SAME DISK as the SQLite database. It therefore does
// NOT survive total-disk failure: a full filesystem, a read-only remount, or dead media
// takes the sink down with the journal. It is not a second copy for durability; it is a
// second *mechanism* for a disjoint set of failures where SQLite specifically fails while
// a dumb O_APPEND write still succeeds:
//
//   * SQLITE_BUSY past the 5 s busy_timeout — another process (a stale miner instance, a
//     backup job, an operator's sqlite3 shell in a transaction) holds the write lock.
//   * A stale POSIX file lock on the DB left by a crashed process.
//   * WAL corruption / SQLITE_CORRUPT / SQLITE_NOTADB — the DB is unusable but the
//     filesystem underneath is fine.
//   * SQLITE_FULL from a page-cache or quota condition that a single small append (a few
//     hundred bytes, no WAL amplification, no page allocation) still slips under.
//   * SQLITE_IOERR from an fsync failing on the WAL specifically.
//
// Failures it does NOT cover: disk gone, filesystem read-only, directory unwritable,
// power loss between the find and the write(2). For those the answer is off-box
// replication, which is out of scope for Phase 1.
//
// FORMAT
// ------
// JSONL: one find, one self-contained JSON object, one line, fsync'd before append()
// returns. Self-contained-per-line is deliberate — a torn tail from a crash mid-write
// costs exactly one record, and importInto() steps over the damage instead of aborting
// recovery (see "malformed lines never block recovery" below).
//
// No JSON library. The treeminer_journal target links sqlite3 and nothing else by design;
// pulling nlohmann_json in here to serialize nine flat fields would put a header-heavy
// dependency on the one code path that has to work when everything else is broken. The
// serializer and parser below are self-contained std-only helpers.
//
// RECOVERY
// --------
// importInto() drains the sink back into the journal at startup. FindJournal::append() is
// idempotent by the UNIQUE key column (it returns the existing row id for a duplicate), so
// re-importing a record that was journaled after all — or imported on a previous boot that
// died before the file could be renamed — is a no-op, not a duplicate submission.
//
// THREADING
// ---------
// append() holds no state and no lock: each call opens, writes, fsyncs and closes its own
// descriptor with O_APPEND, so concurrent callers cannot interleave a record. Finds arrive
// a few times per hour at most, so the per-call open/fsync/close cost is irrelevant and
// durability wins outright.

#include <cstddef>
#include <string>

#include "treeminer/IFindJournal.h"
#include "treeminer/Types.h"

namespace treeminer {

class FallbackSink {
public:
    // Records the sink path only. Does NOT create, open or stat anything: this object is
    // constructed on the miner's startup path, where a filesystem error must not abort
    // boot, and it must be safe to construct even when the disk is the thing that is
    // broken. The file is created lazily on the first append(). Never throws.
    explicit FallbackSink(std::string path) noexcept;

    // Appends one find as a single JSONL line and fsyncs it to stable storage before
    // returning. Returns false on ANY failure (open, write, fsync, close) and never
    // throws — the caller is already in a failure handler and has nowhere to escalate to
    // except a log line. A false return means the find is genuinely unrecoverable and the
    // caller should log at the highest severity it has.
    bool append(const FoundPayload& payload) noexcept;

    const std::string& path() const noexcept { return path_; }

    struct ImportStats {
        std::size_t imported = 0;    // records handed to journal.append() without error
        std::size_t malformed = 0;   // lines that could not be parsed; skipped, not fatal
        bool file_present = false;   // false only when the sink file does not exist
    };

    // Startup drain: reads `path` line by line and appends every parseable record to
    // `journal`.
    //
    // Semantics, all chosen so that recovery degrades gracefully rather than all-or-
    // nothing:
    //   * Missing file — the overwhelmingly common case, since the sink is only written
    //     when SQLite is broken — returns {0, 0, false} with no error and no side effects.
    //   * An unparseable line increments `malformed` and recovery CONTINUES. One corrupt
    //     line (a torn tail, a stray editor save) must never strand the good records
    //     behind it; that would recreate the very drop this component exists to prevent.
    //   * If journal.append() throws, importing STOPS at that record and the stats so far
    //     are returned with file_present=true. The exception is swallowed (this is a boot
    //     path; a broken journal is reported by the caller's own journal health check) and
    //     the sink file is left exactly as-is so the next boot retries the whole thing.
    //     Records already imported in this pass are deduped by key on the retry.
    //   * Only after a clean pass — file read to EOF, every parseable record appended
    //     without error — is the file renamed to "<path>.imported": the evidence survives
    //     for the operator, and the next boot starts from an empty sink. An existing
    //     "<path>.imported" is removed first, because std::rename's behavior over an
    //     existing target is not portable (POSIX replaces, Win32 fails).
    //   * A failed rename is not an error: the records are already durable in the journal
    //     and the next boot re-imports them idempotently.
    static ImportStats importInto(IFindJournal& journal, const std::string& path);

private:
    std::string path_;
};

}  // namespace treeminer
