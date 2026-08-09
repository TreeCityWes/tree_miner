// CircuitBreaker unit tests — deterministic via the injectable monotonic clock and a
// zero-jitter source.

#include <cstdint>

#include "submit/CircuitBreaker.h"
#include "test_framework.h"

using treeminer::CircuitBreaker;
using State = treeminer::CircuitBreaker::State;

namespace {
std::int64_t g_now = 0;
std::int64_t clock_fn() { return g_now; }

CircuitBreaker makeBreaker(CircuitBreaker::Config cfg = {}) {
    return CircuitBreaker(cfg, &clock_fn);
}
}  // namespace

int main() {
    // --- opens after N consecutive transport failures (default 3) ---
    TEST_CASE("opens after 3 consecutive transport failures");
    {
        g_now = 0;
        auto b = makeBreaker();
        CHECK(b.state() == State::Closed);
        CHECK(b.tryAdmit());
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Closed);
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Closed);
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Open);
        CHECK(!b.tryAdmit());
    }

    TEST_CASE("a success or conclusive response resets the failure streak");
    {
        g_now = 0;
        auto b = makeBreaker();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifySuccess();  // streak reset
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Closed);
        b.onVerifyInconclusive();  // 401/4xx classification also resets
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Closed);
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Open);
    }

    // --- OPEN probe schedule: 5s -> x2 -> cap 60s ---
    TEST_CASE("probe schedule doubles from 5s and caps at 60s");
    {
        g_now = 1000;
        auto b = makeBreaker();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Open);
        CHECK_EQ(b.nextProbeAtMs(), 1000 + 5000);
        CHECK(!b.probeDue());
        g_now = 1000 + 5000;
        CHECK(b.probeDue());
        b.onProbeFailure();  // 10s
        CHECK_EQ(b.nextProbeAtMs(), g_now + 10000);
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();  // 20s
        CHECK_EQ(b.nextProbeAtMs(), g_now + 20000);
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();  // 40s
        CHECK_EQ(b.nextProbeAtMs(), g_now + 40000);
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();  // 80s -> capped at 60s
        CHECK_EQ(b.nextProbeAtMs(), g_now + 60000);
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();  // stays capped
        CHECK_EQ(b.nextProbeAtMs(), g_now + 60000);
    }

    TEST_CASE("jitter adds up to jitter_fraction of the interval");
    {
        g_now = 0;
        CircuitBreaker::Config cfg;
        cfg.jitter_fraction = 0.2;
        CircuitBreaker b(cfg, &clock_fn, [] { return 0.5; });  // mid-range jitter
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        // 5000 + 0.5 * 0.2 * 5000 = 5500
        CHECK_EQ(b.nextProbeAtMs(), 5500);
    }

    // --- XUNI pressure caps the probe interval at 5s ---
    TEST_CASE("xuni pressure caps probes at 5s and pulls in far probes");
    {
        g_now = 0;
        auto b = makeBreaker();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();  // 10s
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();  // 20s
        CHECK_EQ(b.nextProbeAtMs(), g_now + 20000);
        b.setXuniPressure(true);  // an eligible XUNI appeared mid-outage
        CHECK(b.nextProbeAtMs() <= g_now + 5000);
        // Subsequent failed probes stay capped at 5s while pressure holds.
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();
        CHECK(b.nextProbeAtMs() <= g_now + 5000);
        b.setXuniPressure(false);
        g_now = b.nextProbeAtMs();
        b.onProbeFailure();
        CHECK(b.nextProbeAtMs() > g_now + 5000);  // back to the escalated interval
    }

    // --- HALF_OPEN admits exactly one real submission ---
    TEST_CASE("half-open admits one submission");
    {
        g_now = 0;
        auto b = makeBreaker();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        g_now = b.nextProbeAtMs();
        CHECK(b.probeDue());
        b.onProbeSuccess();
        CHECK(b.state() == State::HalfOpen);
        CHECK(b.tryAdmit());
        CHECK(!b.tryAdmit());  // only one until the outcome is reported
    }

    // --- closes only on verification-path success ---
    TEST_CASE("half-open closes only on verification success");
    {
        g_now = 0;
        auto b = makeBreaker();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        g_now = b.nextProbeAtMs();
        b.onProbeSuccess();
        CHECK(b.tryAdmit());
        b.onVerifyInconclusive();  // e.g. 401 difficulty park: transport fine, NOT a close
        CHECK(b.state() == State::HalfOpen);
        CHECK(b.tryAdmit());       // slot released for the next drain probe
        b.onVerifySuccess();       // 200 or conclusive duplicate
        CHECK(b.state() == State::Closed);
        CHECK(b.tryAdmit());
    }

    TEST_CASE("half-open transport failure reopens with escalated interval");
    {
        g_now = 0;
        auto b = makeBreaker();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        b.onVerifyTransportFailure();
        g_now = b.nextProbeAtMs();  // 5000
        b.onProbeSuccess();
        CHECK(b.tryAdmit());
        b.onVerifyTransportFailure();  // the half-open probe failed
        CHECK(b.state() == State::Open);
        CHECK(!b.tryAdmit());
        CHECK_EQ(b.nextProbeAtMs(), g_now + 10000);  // escalated beyond the 5s base
    }

    TEST_CASE("custom threshold is honored");
    {
        g_now = 0;
        CircuitBreaker::Config cfg;
        cfg.failure_threshold = 1;
        CircuitBreaker b(cfg, &clock_fn);
        b.onVerifyTransportFailure();
        CHECK(b.state() == State::Open);
    }

    return testfw::summary("circuit_breaker");
}
