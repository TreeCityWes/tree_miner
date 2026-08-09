#include "ResponseClassifier.h"

#include <cctype>
#include <cstdlib>

namespace treeminer {

namespace {

bool isBlank(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

void skipWs(const std::string& s, std::size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
}

// Parse a JSON string literal starting at s[i] == '"'. Advances i past the closing quote.
// Escapes are decoded (\uXXXX outside ASCII becomes '?'; the server never emits those).
std::optional<std::string> parseJsonString(const std::string& s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"') {
        return std::nullopt;
    }
    ++i;
    std::string out;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            ++i;
            return out;
        }
        if (c == '\\') {
            ++i;
            if (i >= s.size()) {
                return std::nullopt;
            }
            char e = s[i];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 >= s.size()) {
                        return std::nullopt;
                    }
                    unsigned code = 0;
                    for (int k = 1; k <= 4; ++k) {
                        char h = s[i + k];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return std::nullopt;
                    }
                    out += (code < 0x80) ? static_cast<char>(code) : '?';
                    i += 4;
                    break;
                }
                default:
                    return std::nullopt;
            }
            ++i;
        } else {
            out += c;
            ++i;
        }
    }
    return std::nullopt;  // unterminated
}

// Skip a JSON value (string, number, literal, object, array) starting at s[i].
bool skipJsonValue(const std::string& s, std::size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) {
        return false;
    }
    char c = s[i];
    if (c == '"') {
        return parseJsonString(s, i).has_value();
    }
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            char d = s[i];
            if (d == '"') {
                if (!parseJsonString(s, i).has_value()) {
                    return false;
                }
                continue;
            }
            if (d == open) ++depth;
            if (d == close) {
                --depth;
                if (depth == 0) {
                    ++i;
                    return true;
                }
            }
            ++i;
        }
        return false;
    }
    // number / true / false / null
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return true;
}

// Capture a scalar value (string or number/literal) as text; nullopt for object/array values.
std::optional<std::string> captureScalar(const std::string& s, std::size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) {
        return std::nullopt;
    }
    if (s[i] == '"') {
        return parseJsonString(s, i);
    }
    if (s[i] == '{' || s[i] == '[') {
        skipJsonValue(s, i);  // structured value: not a scalar
        return std::nullopt;
    }
    std::size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return s.substr(start, i - start);
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

Classification pending(std::string reason) {
    Classification c;
    c.next_status = FindStatus::Pending;
    c.reason = std::move(reason);
    return c;
}

Classification quarantined(std::string reason) {
    Classification c;
    c.next_status = FindStatus::Quarantined;
    c.reason = std::move(reason);
    return c;
}

}  // namespace

std::optional<std::string> extractJsonField(const std::string& body, const std::string& key) {
    std::size_t i = 0;
    skipWs(body, i);
    if (i >= body.size() || body[i] != '{') {
        return std::nullopt;
    }
    ++i;
    skipWs(body, i);
    if (i < body.size() && body[i] == '}') {
        return std::nullopt;  // empty object
    }
    while (i < body.size()) {
        skipWs(body, i);
        auto k = parseJsonString(body, i);
        if (!k) {
            return std::nullopt;
        }
        skipWs(body, i);
        if (i >= body.size() || body[i] != ':') {
            return std::nullopt;
        }
        ++i;
        if (*k == key) {
            return captureScalar(body, i);
        }
        if (!skipJsonValue(body, i)) {
            return std::nullopt;
        }
        skipWs(body, i);
        if (i < body.size() && body[i] == ',') {
            ++i;
            continue;
        }
        break;
    }
    return std::nullopt;
}

std::optional<std::string> extractJsonMessage(const std::string& body) {
    if (auto m = extractJsonField(body, "message")) {
        return m;
    }
    return extractJsonField(body, "error");
}

std::optional<std::uint32_t> parseDifficultyHint(const std::string& message) {
    for (std::size_t pos = message.find("m="); pos != std::string::npos;
         pos = message.find("m=", pos + 1)) {
        std::size_t d = pos + 2;
        if (d >= message.size() || !std::isdigit(static_cast<unsigned char>(message[d]))) {
            continue;
        }
        std::uint64_t value = 0;
        while (d < message.size() && std::isdigit(static_cast<unsigned char>(message[d]))) {
            value = value * 10 + static_cast<std::uint64_t>(message[d] - '0');
            if (value > 0xFFFFFFFFull) {
                return std::nullopt;  // absurd; treat as no hint
            }
            ++d;
        }
        return static_cast<std::uint32_t>(value);
    }
    return std::nullopt;
}

std::optional<long> parseRetryAfterSeconds(const std::string& header_value) {
    std::size_t i = 0;
    skipWs(header_value, i);
    if (i >= header_value.size() || !std::isdigit(static_cast<unsigned char>(header_value[i]))) {
        return std::nullopt;  // HTTP-date form not supported; caller falls back to backoff
    }
    long value = 0;
    while (i < header_value.size() && std::isdigit(static_cast<unsigned char>(header_value[i]))) {
        if (value > 100000000L) {
            return std::nullopt;
        }
        value = value * 10 + (header_value[i] - '0');
        ++i;
    }
    skipWs(header_value, i);
    if (i != header_value.size()) {
        return std::nullopt;
    }
    return value;
}

Classification classify(int http_status, const std::string& body, FindKind kind) {
    return classify(http_status, body, kind, std::nullopt);
}

Classification classify(int http_status, const std::string& body, FindKind kind,
                        const std::optional<std::string>& retry_after) {
    // Transport-level failure (connect error / timeout / DNS): retry forever with backoff.
    if (http_status <= 0) {
        return pending("transport failure; will retry with backoff");
    }

    // Empty body is indistinguishable from a proxy/serving failure — never conclusive.
    if (body.empty() || isBlank(body)) {
        return pending("empty response body (http " + std::to_string(http_status) +
                       "); will retry with backoff");
    }

    // Structured parse first, raw-body substring fallback second.
    const std::string message = extractJsonMessage(body).value_or(body);

    if (http_status == 200) {
        // gpage.py:492-494,515 — the server answers 200 even when its insert retries were
        // exhausted, so a 200 is only "accepted, unconfirmed" until /get_block agrees.
        Classification c;
        c.next_status = FindStatus::AcceptedUnconfirmed;
        c.needs_lookup_confirmation = true;
        c.reason = "http 200; awaiting /get_block confirmation";
        return c;
    }

    if (http_status == 429) {
        Classification c = pending("rate limited (429)");
        if (retry_after) {
            if (auto secs = parseRetryAfterSeconds(*retry_after)) {
                c.reason += "; retry_after_s=" + std::to_string(*secs);
            }
        }
        return c;
    }

    if (http_status == 408 || http_status == 425 || http_status >= 500) {
        return pending("server unhealthy (http " + std::to_string(http_status) +
                       "); will retry with backoff");
    }

    if (http_status == 400) {
        // gpage.py:510 — UNIQUE-key IntegrityError => a prior attempt already landed.
        // `message` is the structured {"message": ...} field when the body is JSON, and
        // the raw body otherwise — substring matching is the fallback, never both.
        if (contains(message, "already exists")) {
            Classification c;
            c.next_status = FindStatus::AcceptedUnconfirmed;
            c.needs_lookup_confirmation = true;
            c.reason = "duplicate key (already exists); confirming via /get_block";
            return c;
        }
        // Validation 400s ({"error": ...}: invalid key/salt/missing fields) and anything
        // unknown: operator-visible, never auto-retried, never silently dropped.
        return quarantined("unrecognized 400: " + message);
    }

    if (http_status == 401) {
        // gpage.py:519 — argon2.verify said no. Retrying cannot fix a bad payload.
        if (contains(message, "Hash verification failed")) {
            Classification c;
            c.next_status = FindStatus::PermanentlyInvalid;
            c.reason = "server rejected: hash verification failed";
            return c;
        }

        // gpage.py:434 (current) and gpage.py:497 (legacy, XUNI-only else-branch).
        if (contains(message, "outside of proper time frame") ||
            contains(message, "outside of time window")) {
            if (kind == FindKind::XUNI) {
                Classification c;
                c.next_status = FindStatus::ParkedXuniWindow;
                c.reason = "XUNI outside server time window; parked for a later window";
                return c;
            }
            // docs/05 §2: a XEN11 submission can never receive this response. If it does,
            // the server has changed — quarantine and make it loud.
            return quarantined(
                "IMPOSSIBLE: XUNI-window rejection for a XEN11 record — server semantics "
                "changed, investigate: " + message);
        }

        // gpage.py:416 — "Hash does not contain 'm={N}'. ..." where N is the CURRENT
        // difficulty. Strictly-< check server-side, so this record is auto-valid again
        // once difficulty falls to <= its m.
        if (auto hint = parseDifficultyHint(message)) {
            Classification c;
            c.next_status = FindStatus::ParkedDifficulty;
            c.server_difficulty_hint = hint;
            c.reason = "difficulty too low (server currently at m=" +
                       std::to_string(*hint) + "); parked until difficulty falls";
            return c;
        }

        return quarantined("unrecognized 401: " + message);
    }

    // Any other status (3xx, other 4xx, unknown schema): quarantine, operator-visible.
    return quarantined("unrecognized response (http " + std::to_string(http_status) +
                       "): " + message);
}

}  // namespace treeminer
