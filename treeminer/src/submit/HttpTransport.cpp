#include "HttpTransport.h"

#include <cctype>
#include <exception>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "../HttpClient.h"
#include "../HttpResponse.h"

namespace treeminer {

namespace {

// Header lookup, case-insensitive on the header name (proxies vary the casing).
std::optional<std::string> findHeader(const std::map<std::string, std::string>& headers,
                                      const std::string& name) {
    for (const auto& kv : headers) {
        if (kv.first.size() != name.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(kv.first[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return kv.second;
        }
    }
    return std::nullopt;
}

TransportResult fromResponse(const HttpResponse& r) {
    TransportResult out;
    // cpr reports status_code 0 for connect errors / timeouts / DNS failures.
    if (r.GetStatusCode() <= 0) {
        out.transport_ok = false;
        out.error = "transport failure (no HTTP response)";
        return out;
    }
    out.transport_ok = true;
    out.http_status = r.GetStatusCode();
    out.body = r.GetBody();
    out.retry_after = findHeader(r.GetHeaders(), "Retry-After");
    out.date_header = findHeader(r.GetHeaders(), "Date");
    return out;
}

TransportResult transportError(const char* what) {
    TransportResult out;
    out.transport_ok = false;
    out.error = what;
    return out;
}

}  // namespace

HttpTransport::HttpTransport(std::string rpc_link, std::string worker,
                             long submit_timeout_ms, long get_timeout_ms)
    : rpc_(std::move(rpc_link)),
      worker_(std::move(worker)),
      submit_timeout_ms_(submit_timeout_ms),
      get_timeout_ms_(get_timeout_ms) {
    while (!rpc_.empty() && rpc_.back() == '/') {
        rpc_.pop_back();
    }
}

TransportResult HttpTransport::submit(const FoundPayload& payload) {
    try {
        // Field-for-field the upstream /verify payload (src/main.cpp:373-380):
        // attempts and hashes_per_second are transmitted as strings.
        std::ostringstream hps;
        hps << std::fixed << std::setprecision(2) << payload.hashes_per_second;
        nlohmann::json body = {
            {"hash_to_verify", payload.hash_to_verify},
            {"key", payload.key},
            {"account", payload.account},
            {"attempts", std::to_string(payload.attempts)},
            {"hashes_per_second", hps.str()},
            {"worker", worker_.empty() ? payload.worker : worker_},
        };
        HttpClient client;
        return fromResponse(client.HttpPost(rpc_ + "/verify", body, submit_timeout_ms_));
    } catch (const std::exception&) {
        return transportError("exception during POST /verify");
    } catch (...) {
        return transportError("unknown exception during POST /verify");
    }
}

TransportResult HttpTransport::confirm(const std::string& key) {
    try {
        // key is 64-hex — URL-safe by construction, no escaping needed.
        HttpClient client;
        return fromResponse(client.HttpGet(rpc_ + "/get_block?key=" + key, get_timeout_ms_));
    } catch (const std::exception&) {
        return transportError("exception during GET /get_block");
    } catch (...) {
        return transportError("unknown exception during GET /get_block");
    }
}

TransportResult HttpTransport::difficulty() {
    try {
        HttpClient client;
        return fromResponse(client.HttpGet(rpc_ + "/difficulty", get_timeout_ms_));
    } catch (const std::exception&) {
        return transportError("exception during GET /difficulty");
    } catch (...) {
        return transportError("unknown exception during GET /difficulty");
    }
}

}  // namespace treeminer
