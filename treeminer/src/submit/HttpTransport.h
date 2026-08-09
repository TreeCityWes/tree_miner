#pragma once
// HttpTransport — thin ITransport adapter over the upstream HttpClient (cpr).
//
// This is the ONLY file in src/submit that touches upstream code; it is compiled only
// when TREEMINER_SUBMIT_WITH_HTTP is ON in CMake (the unit-test build never sees it, so
// the rest of the library stays pure std and builds without CUDA/vcpkg).

#include <string>

#include "ITransport.h"

namespace treeminer {

class HttpTransport : public ITransport {
public:
    // `rpc_link` e.g. "http://xenblocks.io"; `worker` is the machine id sent in the
    // /verify payload (upstream "worker" field). Timeouts are hard totals per request
    // (the upstream HttpClient exposes a single total timeout; PLAN §3.2 asks 10 s for
    // /verify and 5 s for GETs).
    HttpTransport(std::string rpc_link, std::string worker,
                  long submit_timeout_ms = 10000, long get_timeout_ms = 5000);

    TransportResult submit(const FoundPayload& payload) override;
    TransportResult confirm(const std::string& key) override;
    TransportResult difficulty() override;

private:
    std::string rpc_;
    std::string worker_;
    long submit_timeout_ms_;
    long get_timeout_ms_;
};

}  // namespace treeminer
