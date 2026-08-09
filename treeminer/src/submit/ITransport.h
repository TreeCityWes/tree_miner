#pragma once
// Transport boundary for the submission layer (SOL-PLAN §8; PLAN.md risk 1: an X1
// migration replaces only the adapter behind this interface, never the journal or the
// state machine). Pure std; the HTTP implementation lives in HttpTransport.{h,cpp} and is
// compiled only when the miner build enables it.

#include <optional>
#include <string>

#include "../treeminer/Types.h"

namespace treeminer {

// Outcome of one transport round-trip. `transport_ok == false` means the request never
// produced an HTTP response (connect error, timeout, DNS failure); http_status/body are
// meaningless in that case and `error` describes the failure.
struct TransportResult {
    bool transport_ok = false;
    int http_status = 0;
    std::string body;
    std::optional<std::string> retry_after;  // raw Retry-After header, when present
    std::optional<std::string> date_header;  // raw HTTP Date header, for server-clock offset
    std::string error;                       // transport-level failure description
};

class ITransport {
public:
    virtual ~ITransport() = default;

    // POST /verify with the immutable payload. Must apply hard timeouts internally and
    // never throw: every failure comes back as transport_ok == false.
    virtual TransportResult submit(const FoundPayload& payload) = 0;

    // GET /get_block?key=<key> — confirmation lookup for AcceptedUnconfirmed
    // (gpage.py:331-364: 200 with the stored row, 404 when absent).
    virtual TransportResult confirm(const std::string& key) = 0;

    // GET /difficulty — breaker probe + difficulty observation
    // (gpage.py:109-117: {"difficulty": "<N>"}).
    virtual TransportResult difficulty() = 0;
};

}  // namespace treeminer
