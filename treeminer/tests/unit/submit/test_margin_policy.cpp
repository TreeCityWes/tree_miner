// Unit tests for the difficulty-margin policy (src/treeminer/MarginPolicy.h).
//
// The ramp is grounded in the server's own adjustment rule: manage_difficulty2.py moves
// difficulty at most +1000 KiB per 300 s tick (docs/05 §1), so one step of headroom per
// adjustment period is the exact worst-case rise. These tests pin that arithmetic, the
// "healthy costs nothing" guarantee, and the ceiling.

#include "treeminer/MarginPolicy.h"

#include <cstring>
#include <string>

#include "test_framework.h"

using namespace treeminer;

namespace {

MarginConfig autoConfig() {
    MarginConfig cfg;
    cfg.mode = MarginMode::Auto;
    cfg.margin_kib = 1000;
    cfg.max_kib = 5000;
    cfg.adjust_period_ms = 300000;  // 5 minutes
    return cfg;
}

MarginInputs healthy() {
    MarginInputs in;
    in.breaker_open = false;
    in.outage_ms = 0;
    in.backlog = 0;
    return in;
}

MarginInputs outage(std::int64_t ms) {
    MarginInputs in;
    in.breaker_open = true;
    in.outage_ms = ms;
    in.backlog = 0;
    return in;
}

}  // namespace

int main() {
    // --- Off mode: the feature must be inert until asked for ---
    TEST_CASE("off mode never adds headroom");
    {
        MarginConfig cfg;  // default is Off
        CHECK_EQ(cfg.mode == MarginMode::Off, true);
        CHECK_EQ(computeMargin(cfg, healthy()), 0u);
        CHECK_EQ(computeMargin(cfg, outage(3600000)), 0u);
        MarginInputs backlogged = healthy();
        backlogged.backlog = 500;
        CHECK_EQ(computeMargin(cfg, backlogged), 0u);
    }

    // --- Fixed mode: constant, taken at face value ---
    TEST_CASE("fixed mode is constant regardless of health");
    {
        MarginConfig cfg;
        cfg.mode = MarginMode::Fixed;
        cfg.margin_kib = 2500;
        CHECK_EQ(computeMargin(cfg, healthy()), 2500u);
        CHECK_EQ(computeMargin(cfg, outage(9999999)), 2500u);
    }

    TEST_CASE("fixed mode is not clamped by the auto ceiling");
    {
        // max_kib governs the auto ramp only. An operator who writes an explicit constant
        // gets that constant; silently mining at a different cost would be worse.
        MarginConfig cfg;
        cfg.mode = MarginMode::Fixed;
        cfg.margin_kib = 9000;
        cfg.max_kib = 5000;
        CHECK_EQ(computeMargin(cfg, healthy()), 9000u);
    }

    // --- Auto mode: the whole point is that healthy operation is untaxed ---
    TEST_CASE("auto mode costs nothing while healthy and drained");
    {
        CHECK_EQ(computeMargin(autoConfig(), healthy()), 0u);
    }

    TEST_CASE("auto mode buys one step the moment the breaker opens");
    {
        // A find made right now cannot be submitted right now, so it is already exposed —
        // headroom applies immediately, not after the first adjustment period elapses.
        CHECK_EQ(computeMargin(autoConfig(), outage(0)), 1000u);
        CHECK_EQ(computeMargin(autoConfig(), outage(1)), 1000u);
    }

    TEST_CASE("auto ramp adds one step per difficulty adjustment period");
    {
        const MarginConfig cfg = autoConfig();
        CHECK_EQ(computeMargin(cfg, outage(299999)), 1000u);   // < 1 period
        CHECK_EQ(computeMargin(cfg, outage(300000)), 2000u);   // exactly 1 period
        CHECK_EQ(computeMargin(cfg, outage(600000)), 3000u);   // 2 periods
        CHECK_EQ(computeMargin(cfg, outage(900000)), 4000u);   // 3 periods
    }

    TEST_CASE("auto ramp stops at the ceiling");
    {
        const MarginConfig cfg = autoConfig();  // max 5000
        CHECK_EQ(computeMargin(cfg, outage(1200000)), 5000u);   // 4 periods -> 5000
        CHECK_EQ(computeMargin(cfg, outage(1500000)), 5000u);   // would be 6000, capped
        CHECK_EQ(computeMargin(cfg, outage(86400000)), 5000u);  // a full day, still capped
    }

    TEST_CASE("auto mode holds headroom while a backlog drains after recovery");
    {
        // The server is reachable again but finds are still queued: difficulty may have
        // climbed during the outage, so newly mined finds still need headroom until the
        // journal is clear.
        MarginInputs recovering = healthy();
        recovering.backlog = 1;
        CHECK_EQ(computeMargin(autoConfig(), recovering), 1000u);

        // Outage duration does not inflate the margin once the breaker has closed — only
        // the backlog keeps it alive, at one step.
        recovering.backlog = 10000;
        CHECK_EQ(computeMargin(autoConfig(), recovering), 1000u);
    }

    TEST_CASE("auto mode returns to zero once the backlog clears");
    {
        MarginInputs drained = healthy();
        drained.backlog = 0;
        CHECK_EQ(computeMargin(autoConfig(), drained), 0u);
    }

    TEST_CASE("step size is configurable and multiplies through the ramp");
    {
        MarginConfig cfg = autoConfig();
        cfg.margin_kib = 500;
        cfg.max_kib = 100000;
        CHECK_EQ(computeMargin(cfg, outage(0)), 500u);
        CHECK_EQ(computeMargin(cfg, outage(300000)), 1000u);
        CHECK_EQ(computeMargin(cfg, outage(1500000)), 3000u);
    }

    TEST_CASE("zero step size yields no headroom even while degraded");
    {
        MarginConfig cfg = autoConfig();
        cfg.margin_kib = 0;
        CHECK_EQ(computeMargin(cfg, outage(600000)), 0u);
    }

    TEST_CASE("a huge step cannot overflow the returned headroom");
    {
        MarginConfig cfg = autoConfig();
        cfg.margin_kib = 4000000000u;  // absurd, but must not wrap
        cfg.max_kib = 5000;
        CHECK_EQ(computeMargin(cfg, outage(3000000)), 5000u);
    }

    TEST_CASE("a zero adjust period does not divide by zero");
    {
        MarginConfig cfg = autoConfig();
        cfg.adjust_period_ms = 0;
        CHECK_EQ(computeMargin(cfg, outage(600000)), 1000u);  // one step, no ramp
    }

    // --- config parsing ---
    TEST_CASE("margin mode parses the documented spellings");
    {
        MarginMode m = MarginMode::Fixed;
        CHECK(parseMarginMode("off", m));
        CHECK(m == MarginMode::Off);
        CHECK(parseMarginMode("AUTO", m));
        CHECK(m == MarginMode::Auto);
        CHECK(parseMarginMode("Fixed", m));
        CHECK(m == MarginMode::Fixed);
        CHECK(parseMarginMode("adaptive", m));
        CHECK(m == MarginMode::Auto);
    }

    TEST_CASE("an unknown margin mode is rejected, never defaulted");
    {
        // Silently falling back would mine at a memory cost the operator did not ask for.
        MarginMode m = MarginMode::Auto;
        CHECK(!parseMarginMode("aggressive", m));
        CHECK(!parseMarginMode("", m));
        CHECK(!parseMarginMode("1000", m));
        CHECK(m == MarginMode::Auto);  // left untouched on failure
    }

    TEST_CASE("mode round-trips through to_string");
    {
        CHECK(std::strcmp(to_string(MarginMode::Off), "off") == 0);
        CHECK(std::strcmp(to_string(MarginMode::Fixed), "fixed") == 0);
        CHECK(std::strcmp(to_string(MarginMode::Auto), "auto") == 0);
        MarginMode m = MarginMode::Off;
        CHECK(parseMarginMode(to_string(MarginMode::Auto), m));
        CHECK(m == MarginMode::Auto);
    }

    return testfw::summary("margin_policy");
}
