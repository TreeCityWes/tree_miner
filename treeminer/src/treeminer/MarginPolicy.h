#pragma once
// Difficulty-margin policy — PLAN.md §5 (config), §10.7 (adaptive presets).
//
// WHY HEADROOM EXISTS
// The server rejects a submission only when the m baked into the hash is strictly BELOW the
// difficulty current at submit time (gpage.py:404,412 — docs/05 §1, docs/06 A.3). A find is
// therefore not bound to the difficulty it was mined at: it stays valid for as long as the
// network difficulty has not climbed past its m. Mining at `difficulty + margin` buys exactly
// that: tolerance against difficulty rising between discovery and a successful submission.
//
// WHY THE STEP IS 1000 KiB PER 300 s
// manage_difficulty2.py re-evaluates every 300 s and moves by at most +1000 KiB per tick
// (docs/05 §1). One step of headroom per adjustment period is therefore the exact worst-case
// rise — not a guess. A find carrying N steps survives N × 300 s of continuously rising
// difficulty; anything beyond that would have been rejected no matter how it was scheduled.
//
// WHY IT IS NOT ALWAYS ON
// m IS the Argon2 memory cost, so headroom is paid for in hashrate, roughly in proportion
// (docs/06 B.2: H/s <= BW / (2 KiB x m)). Auto mode therefore holds the margin at zero
// whenever the server is reachable and the journal is drained — no healthy-state tax — and
// buys insurance only while finds are actually exposed. Outages usually push difficulty DOWN
// (-2000 per tick), which is why the journal alone already recovers most value; the margin is
// insurance for the partial-outage and recovery-spike cases, not the common path.
//
// This header is pure: no clocks, no I/O, no journal. The caller supplies observations and
// gets a number back, which makes every rung of the ramp unit-testable without sleeping.

#include <cstdint>
#include <string>

namespace treeminer {

enum class MarginMode {
    Off,    // never add headroom (default — byte-for-byte the pre-margin behavior)
    Fixed,  // always add `margin_kib`, healthy or not
    Auto,   // zero while healthy; ramps while the breaker is open or a backlog exists
};

struct MarginConfig {
    MarginMode mode = MarginMode::Off;

    // Fixed mode: the constant headroom in KiB.
    // Auto mode:  the size of ONE escalation step (one difficulty adjustment period's rise).
    std::uint32_t margin_kib = 1000;

    // Auto mode only: ceiling on the ramp. Fixed mode is taken at face value — an operator
    // who writes a number is not second-guessed.
    std::uint32_t max_kib = 5000;

    // Server difficulty adjustment period (manage_difficulty2.py: 300 s).
    std::int64_t adjust_period_ms = 300000;
};

// Observations the policy reacts to. All supplied by the caller; the policy owns no state.
struct MarginInputs {
    bool breaker_open = false;   // /verify path is down (submissions cannot land right now)
    std::int64_t outage_ms = 0;  // how long it has been down; ignored unless breaker_open
    std::size_t backlog = 0;     // finds journaled but not yet terminal (at-risk population)
};

// Returns the headroom in KiB to add to the mined memory cost.
//
// Auto: 0 when healthy (breaker closed AND backlog empty). Otherwise one step immediately —
// a find made right now is already at risk — plus one further step per elapsed adjustment
// period of outage, capped at max_kib.
std::uint32_t computeMargin(const MarginConfig& cfg, const MarginInputs& in);

// Config parsing: "off" | "fixed" | "auto" (case-insensitive). False on anything else, so a
// typo in config.txt is reported rather than silently mining at an unintended memory cost.
bool parseMarginMode(const std::string& text, MarginMode& out);

const char* to_string(MarginMode mode);

}  // namespace treeminer
