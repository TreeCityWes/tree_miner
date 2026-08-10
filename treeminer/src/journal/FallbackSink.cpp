// FallbackSink.cpp — implementation of the last-resort JSONL find sink.
// Rationale, failure-domain coverage and recovery semantics: FallbackSink.h.
//
// Everything in this file is deliberately dependency-free beyond the C++ standard library
// and the platform's raw file API. This is the code path that runs when SQLite is already
// broken; it must not be able to fail for a reason of its own.

#include "journal/FallbackSink.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace treeminer {
namespace {

// ---------------------------------------------------------------------------------------
// Minimal JSON serialization / parsing.
//
// Scope is exactly one flat object of string and number fields — the shape of
// FoundPayload — and nothing more. src/submit/ResponseClassifier.cpp carries a similar
// hand-rolled parser for server responses; these helpers are intentionally a separate,
// self-contained copy rather than a shared one, because that parser lives in a different
// static library (treeminer_submit) and treeminer_journal must not grow a link edge to it
// just to recover finds at boot.
// ---------------------------------------------------------------------------------------

// Escapes '"', '\' and every C0 control character. Bytes >= 0x80 are emitted VERBATIM and
// never \u-escaped: the sink's job is byte-exact round-tripping of whatever the miner
// captured, and re-encoding would require assuming the input is valid UTF-8. In practice
// these fields are hex and ASCII, but "in practice" is not a durability guarantee.
void appendJsonString(std::string& out, const std::string& value) {
    out.push_back('"');
    for (const char rawChar : value) {
        const unsigned char c = static_cast<unsigned char>(rawChar);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(rawChar);
                }
                break;
        }
    }
    out.push_back('"');
}

void appendField(std::string& out, const char* key, const std::string& value, bool first) {
    if (!first) out.push_back(',');
    appendJsonString(out, key);
    out.push_back(':');
    appendJsonString(out, value);
}

void appendRawField(std::string& out, const char* key, const std::string& literal,
                    bool first) {
    if (!first) out.push_back(',');
    appendJsonString(out, key);
    out.push_back(':');
    out += literal;
}

void skipWs(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
        ++i;
    }
}

// Encodes one code point as UTF-8. Only reached for \uXXXX escapes, which this serializer
// never produces above 0x1F — it exists so a hand-edited or foreign-produced sink file
// still imports rather than being written off as malformed.
void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool hexNibble(char c, unsigned int& value) {
    if (c >= '0' && c <= '9') { value = static_cast<unsigned int>(c - '0');      return true; }
    if (c >= 'a' && c <= 'f') { value = static_cast<unsigned int>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { value = static_cast<unsigned int>(c - 'A' + 10); return true; }
    return false;
}

// On success `i` lands one past the closing quote. An unterminated string — the exact
// shape of a torn tail from a crash mid-write — returns false, which is what makes the
// truncated last line of a sink file "malformed" rather than fatal.
bool parseJsonString(const std::string& s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            ++i;
            if (i >= s.size()) return false;
            const char esc = s[i++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        unsigned int nibble = 0;
                        if (!hexNibble(s[i + static_cast<std::size_t>(k)], nibble)) {
                            return false;
                        }
                        cp = (cp << 4) | nibble;
                    }
                    i += 4;
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return false;  // unknown escape: treat the whole line as damaged
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return false;  // ran off the end without a closing quote
}

// Parses one flat {"k":"v","k2":123} object into a key -> raw-text map. Values are either
// JSON strings (unescaped into the map) or bare numeric/literal tokens (kept verbatim for
// the caller to convert). Nested objects and arrays are rejected outright: nothing in
// FoundPayload is structured, so encountering one means the line is not ours.
bool parseFlatObject(const std::string& line, std::map<std::string, std::string>& fields) {
    const std::size_t n = line.size();
    std::size_t i = 0;
    skipWs(line, i);
    if (i >= n || line[i] != '{') return false;
    ++i;
    skipWs(line, i);

    if (i < n && line[i] == '}') {
        ++i;
        skipWs(line, i);
        return i == n;  // syntactically valid but fieldless; the caller rejects it
    }

    for (;;) {
        skipWs(line, i);
        std::string key;
        if (!parseJsonString(line, i, key)) return false;
        skipWs(line, i);
        if (i >= n || line[i] != ':') return false;
        ++i;
        skipWs(line, i);
        if (i >= n) return false;

        std::string value;
        if (line[i] == '"') {
            if (!parseJsonString(line, i, value)) return false;
        } else if (line[i] == '{' || line[i] == '[') {
            return false;
        } else {
            const std::size_t start = i;
            while (i < n && line[i] != ',' && line[i] != '}' && line[i] != ' ' &&
                   line[i] != '\t' && line[i] != '\r' && line[i] != '\n') {
                ++i;
            }
            if (i == start) return false;
            value.assign(line, start, i - start);
        }
        fields[std::move(key)] = std::move(value);

        skipWs(line, i);
        if (i < n && line[i] == ',') {
            ++i;
            continue;
        }
        break;
    }

    if (i >= n || line[i] != '}') return false;  // truncated before the closing brace
    ++i;
    skipWs(line, i);
    return i == n;  // trailing junk after the object means the line is not trustworthy
}

bool lookup(const std::map<std::string, std::string>& fields, const char* key,
            std::string& out) {
    const auto it = fields.find(key);
    if (it == fields.end()) return false;
    out = it->second;
    return true;
}

bool parseU64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || text[0] == '-') return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool parseDouble(const std::string& text, double& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    out = value;  // ERANGE on subnormals is tolerable; the field is a stats-only rate
    return true;
}

// Every field must be present. A record missing one is a record we cannot faithfully
// reconstruct, and silently substituting defaults would corrupt the audit trail that is
// this file's entire reason to exist.
bool payloadFromFields(const std::map<std::string, std::string>& fields,
                       FoundPayload& payload) {
    std::string kindText, memoryCostText, attemptsText, rateText;
    if (!lookup(fields, "key", payload.key)) return false;
    if (!lookup(fields, "hash_to_verify", payload.hash_to_verify)) return false;
    if (!lookup(fields, "account", payload.account)) return false;
    if (!lookup(fields, "kind", kindText)) return false;
    if (!lookup(fields, "memory_cost", memoryCostText)) return false;
    if (!lookup(fields, "worker", payload.worker)) return false;
    if (!lookup(fields, "attempts", attemptsText)) return false;
    if (!lookup(fields, "hashes_per_second", rateText)) return false;
    if (!lookup(fields, "found_at_utc", payload.found_at_utc)) return false;

    if (kindText == "XEN11") {
        payload.kind = FindKind::XEN11;
    } else if (kindText == "XUNI") {
        payload.kind = FindKind::XUNI;
    } else {
        return false;
    }

    std::uint64_t memoryCost = 0;
    if (!parseU64(memoryCostText, memoryCost) || memoryCost > 0xFFFFFFFFull) return false;
    payload.memory_cost = static_cast<std::uint32_t>(memoryCost);

    if (!parseU64(attemptsText, payload.attempts)) return false;
    if (!parseDouble(rateText, payload.hashes_per_second)) return false;
    return true;
}

std::string serialize(const FoundPayload& payload) {
    std::string out;
    out.reserve(512);
    out.push_back('{');
    appendField(out, "key", payload.key, /*first=*/true);
    appendField(out, "hash_to_verify", payload.hash_to_verify, false);
    appendField(out, "account", payload.account, false);
    appendField(out, "kind", to_string(payload.kind), false);
    appendRawField(out, "memory_cost", std::to_string(payload.memory_cost), false);
    appendField(out, "worker", payload.worker, false);
    appendRawField(out, "attempts", std::to_string(payload.attempts), false);

    // %.17g is the shortest fixed precision that round-trips every IEEE-754 double
    // exactly. std::to_string would silently truncate to 6 decimals, which would make the
    // imported record differ from the one that was captured — unacceptable for a file
    // whose purpose is evidence.
    char rate[64];
    const int written = std::snprintf(rate, sizeof(rate), "%.17g", payload.hashes_per_second);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(rate)) {
        appendRawField(out, "hashes_per_second", "0", false);  // non-finite / absurd
    } else {
        // NaN/inf have no JSON spelling; store 0 rather than emit an unparseable line.
        const std::string rateText(rate);
        const bool numeric = rateText.find_first_of("ni") == std::string::npos &&
                             rateText.find_first_of("NI") == std::string::npos;
        appendRawField(out, "hashes_per_second", numeric ? rateText : std::string("0"),
                       false);
    }

    appendField(out, "found_at_utc", payload.found_at_utc, false);
    out.push_back('}');
    out.push_back('\n');
    return out;
}

// ---------------------------------------------------------------------------------------
// Raw file I/O. Deliberately not iostreams: we need a real descriptor to fsync, and we
// need to know that the bytes are on stable storage before append() returns.
// ---------------------------------------------------------------------------------------

#ifdef _WIN32
int openAppend(const char* path, bool exclusive) {
    const int flags = _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY |
                      (exclusive ? _O_EXCL : 0);
    return ::_open(path, flags, _S_IREAD | _S_IWRITE);
}
int writeAll(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const int n = ::_write(fd, data + offset, static_cast<unsigned int>(size - offset));
        if (n <= 0) return -1;
        offset += static_cast<std::size_t>(n);
    }
    return 0;
}
int syncFd(int fd)  { return ::_commit(fd); }
int closeFd(int fd) { return ::_close(fd); }
#else
int openAppend(const char* path, bool exclusive) {
    // 0600: the `key` field IS the mining secret for that find. Nothing but this process
    // (and root) has any business reading the sink.
    const int flags = O_WRONLY | O_CREAT | O_APPEND | (exclusive ? O_EXCL : 0);
    int fd = -1;
    do {
        fd = ::open(path, flags, S_IRUSR | S_IWUSR);
    } while (fd < 0 && errno == EINTR);
    return fd;
}
int writeAll(int fd, const char* data, std::size_t size) {
    // O_APPEND makes the seek-and-write atomic against other appenders, so records from
    // concurrent callers cannot interleave. A short write is still possible on a signal or
    // a near-full device; we loop, and a crash inside the loop leaves a torn line that
    // importInto() counts as malformed and steps over.
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t n = ::write(fd, data + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        offset += static_cast<std::size_t>(n);
    }
    return 0;
}
int syncFd(int fd) {
    int rc = -1;
    do {
        rc = ::fsync(fd);
    } while (rc < 0 && errno == EINTR);
    return rc;
}
int closeFd(int fd) {
    // Do NOT retry close() on EINTR: on Linux the descriptor is already released and a
    // retry could close an unrelated fd opened by another thread in the meantime.
    return ::close(fd);
}

// fsync on the file alone does not make a NEWLY CREATED file's directory entry durable —
// after a power loss the data blocks can exist with no name pointing at them. Only needed
// the first time the sink is created; failures are non-fatal (the record itself is
// already fsync'd, and a nameless-but-committed file is a strictly better outcome than a
// dropped find).
void syncParentDirectory(const std::string& filePath) {
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
    if (parent.empty()) parent = std::filesystem::path(".");
    int dirFd = -1;
    do {
        dirFd = ::open(parent.c_str(), O_RDONLY);
    } while (dirFd < 0 && errno == EINTR);
    if (dirFd < 0) return;
    (void)syncFd(dirFd);
    (void)::close(dirFd);
    (void)ec;
}
#endif

}  // namespace

FallbackSink::FallbackSink(std::string path) noexcept : path_(std::move(path)) {
    // Intentionally empty: no open, no stat, no mkdir. See the header — this runs during
    // startup and on the failure path, and must not be able to throw or block.
}

bool FallbackSink::append(const FoundPayload& payload) noexcept {
    try {
        const std::string line = serialize(payload);

        // O_EXCL first so we learn whether we created the file, which decides whether the
        // parent directory also needs an fsync. Falling back to a plain append-open covers
        // both "it already existed" and any other error (which the second open reports).
        bool created = true;
        int fd = openAppend(path_.c_str(), /*exclusive=*/true);
        if (fd < 0) {
            created = false;
            fd = openAppend(path_.c_str(), /*exclusive=*/false);
        }
        if (fd < 0) return false;

        bool ok = writeAll(fd, line.data(), line.size()) == 0;
        if (ok) ok = syncFd(fd) == 0;
        // close() can surface a deferred write error; a successful fsync above has already
        // committed the bytes, so treat a close failure as failure only if it is the first
        // error we have seen.
        const int closeResult = closeFd(fd);
        if (ok && closeResult != 0) ok = false;

#ifndef _WIN32
        if (ok && created) syncParentDirectory(path_);
#else
        (void)created;
#endif
        return ok;
    } catch (...) {
        // std::bad_alloc from serialize() is the only realistic path here. noexcept
        // contract: report failure, never propagate into the caller's failure handler.
        return false;
    }
}

FallbackSink::ImportStats FallbackSink::importInto(IFindJournal& journal,
                                                   const std::string& path) {
    ImportStats stats;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return stats;  // {0, 0, false}: the normal case — SQLite never failed
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        // The file exists but is unreadable (permissions, or another process holding it on
        // Windows). Report it as present so the caller can surface "sink pending" rather
        // than claiming a clean boot; do not rename, so the next boot retries.
        stats.file_present = true;
        return stats;
    }
    stats.file_present = true;

    bool cleanPass = true;
    std::string line;
    while (std::getline(in, line)) {
        // A sink written on Windows (or hand-edited there) can carry CR before LF; strip
        // it rather than fail an otherwise-good record on a line-ending detail.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        // Blank lines are not damage — they are what a truncated final write plus a later
        // append can leave behind. Ignore them silently.
        if (line.find_first_not_of(" \t") == std::string::npos) continue;

        std::map<std::string, std::string> fields;
        FoundPayload payload;
        if (!parseFlatObject(line, fields) || !payloadFromFields(fields, payload)) {
            ++stats.malformed;
            continue;
        }

        try {
            (void)journal.append(payload);
            ++stats.imported;
        } catch (...) {
            // The journal is still broken. Stop here, leave the file untouched, and let
            // the next boot retry the whole file — every record already imported in this
            // pass dedupes on its UNIQUE key, so replay is free of side effects.
            cleanPass = false;
            break;
        }
    }

    // getline stopping for any reason other than EOF (a read error mid-file) means we have
    // not seen the whole sink; keeping the file is the safe choice.
    if (!in.eof()) cleanPass = false;
    in.close();

    if (!cleanPass) return stats;

    // Clean pass: archive the file so the evidence survives but the next boot starts from
    // an empty sink. remove-then-rename because std::filesystem::rename over an existing
    // target is POSIX-replaces / Win32-fails. Any failure here is harmless — the records
    // are durable in the journal and a replay would dedupe.
    const std::string archived = path + ".imported";
    std::error_code removeEc, renameEc;
    std::filesystem::remove(archived, removeEc);
    std::filesystem::rename(path, archived, renameEc);
    return stats;
}

}  // namespace treeminer
