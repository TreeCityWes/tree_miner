#pragma once
// SubmissionManager — the single thread that drains the durable journal to the server
// (PLAN.md §3.2 and §10 amendments 2, 4, 5, 6). Replaces upstream BlockSubmitter.
//
// One scheduling step (runOnce, also driven directly by unit tests):
//   breaker OPEN   -> GET /difficulty probe when due (records difficulty, tracks server
//                     clock offset from the HTTP Date header); success arms HALF_OPEN.
//   otherwise      -> fetchEligible -> DrainScheduler picks a record (XUNI window +
//                     difficulty trend aware) -> breaker admission -> transport.submit ->
//                     classify -> optional /get_block confirmation -> journal.recordAttempt
//                     -> breaker/pacing updates -> difficulty hints propagated.
//
// Confirmation-aware acks (PLAN §10.2): a 200 or duplicate is AcceptedUnconfirmed until
// GET /get_block?key= finds the row (-> Acked). A 404 on lookup after a 200 is the
// server's lying-200 (insert retries exhausted, gpage.py:492-494,515): the record goes
// back to Pending and is resubmitted. If the lookup itself is unavailable, the record
// stays AcceptedUnconfirmed with a backed-off next_attempt_at and is re-driven later via
// fetchAwaitingConfirmation (contract v1.1) — never silently presented as confirmed.
// Confirmation retries run after the normal drain step, outside the drain-rate budget,
// and are skipped entirely while the breaker is OPEN.
//
// All time is injectable: a monotonic ms clock for pacing/breaker and a wall epoch-ms
// clock for journal timestamps and the XUNI window estimate.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "../treeminer/IFindJournal.h"
#include "../treeminer/MarginPolicy.h"
#include "../treeminer/Types.h"
#include "CircuitBreaker.h"
#include "DrainScheduler.h"
#include "ITransport.h"
#include "ResponseClassifier.h"

namespace treeminer {

class SubmissionManager {
public:
    struct Config {
        std::size_t fetch_limit = 16;
        std::size_t confirm_fetch_limit = 4;    // AcceptedUnconfirmed retry batch per step
        std::int64_t backoff_base_ms = 2000;    // per-record: base * 2^attempts, capped
        std::int64_t backoff_cap_ms = 300000;
        std::int32_t xuni_max_windows = 3;      // window budget before Dead (PLAN §10.5)
        std::int64_t idle_poll_ms = 250;        // thread wakeup granularity
        CircuitBreaker::Config breaker;
        DrainScheduler::Config drain;

        // Difficulty headroom baked into newly mined hashes (PLAN §5, §10.7). Default Off:
        // the miner behaves exactly as it did before margins existed until an operator asks
        // for insurance. See src/treeminer/MarginPolicy.h.
        MarginConfig margin;
        // How often the auto ramp is re-evaluated. Auto mode reads journal counts, so this is
        // deliberately coarse — the ramp moves on a 300 s scale, not a 250 ms one.
        std::int64_t margin_eval_interval_ms = 5000;
    };

    enum class StepResult {
        Idle,            // nothing due (no eligible work / pacing gate / probe not due)
        Probed,          // OPEN: issued a /difficulty probe
        Submitted,       // issued a /verify attempt (result recorded in the journal)
        BreakerBlocked,  // eligible work exists but the breaker refused admission
        ConfirmRetried,  // no submission was due, but confirmation retries were driven
    };

    using MonotonicClock = std::function<std::int64_t()>;  // ms
    using WallClock = std::function<std::int64_t()>;       // epoch ms, UTC

    // Default clocks (std::chrono) are used when null. Split constructors instead of
    // `Config cfg = Config{}`: GCC rejects that default argument inside the enclosing class.
    SubmissionManager(IFindJournal& journal, ITransport& transport);
    SubmissionManager(IFindJournal& journal, ITransport& transport, Config cfg,
                      MonotonicClock monotonic = nullptr, WallClock wall = nullptr);
    ~SubmissionManager();

    SubmissionManager(const SubmissionManager&) = delete;
    SubmissionManager& operator=(const SubmissionManager&) = delete;

    // --- threading ---
    void start();
    void stop();
    void notifyFindAppended();  // wake the drain thread after journal.append

    // One scheduling step; the thread loop calls this, and tests drive it directly.
    StepResult runOnce();

    // --- difficulty integration (PLAN §3.3, §10.4) ---
    // Fired for every fresh difficulty observation (poller-independent hints from 401
    // bodies and OPEN-state probes) so the engine's cache updates immediately.
    void setDifficultyHintCallback(std::function<void(std::uint32_t)> cb);
    // Called by the DifficultyService poller too, so trend tracking sees every sample.
    void observeDifficulty(std::uint32_t difficulty);
    DifficultyTrend difficultyTrend() const { return trend_; }
    std::optional<std::uint32_t> lastObservedDifficulty() const;

    // --- difficulty margin (PLAN §5, §10.7) ---
    // Fired whenever the headroom the miner should bake into new hashes changes. The mine
    // loop restarts its batch on a change, so this is intentionally low-frequency.
    void setMarginCallback(std::function<void(std::uint32_t)> cb);
    // Headroom in KiB currently in effect. Safe to read from any thread.
    std::uint32_t marginInEffect() const { return margin_kib_.load(); }
    // Milliseconds the /verify path has been OPEN, or 0 when it is not. Any thread.
    std::int64_t outageDurationMs() const;

    // --- server clock (PLAN §10.5; SOL §7) ---
    // Offset = server wall clock - local wall clock, from HTTP Date headers. Unknown
    // until the first dated response.
    std::optional<std::int64_t> serverClockOffsetMs() const;

    struct Metrics {
        std::uint64_t submitted = 0;
        std::uint64_t resubmitted = 0;          // /verify attempts for records tried before
        std::uint64_t acked = 0;
        std::uint64_t accepted_unconfirmed = 0;
        std::uint64_t reconciled_via_get_block = 0;
        std::uint64_t confirmation_retries = 0;
        std::uint64_t lying_200_detected = 0;
        std::uint64_t parked_difficulty = 0;
        std::uint64_t parked_xuni = 0;
        std::uint64_t quarantined = 0;
        std::uint64_t permanently_invalid = 0;
        std::uint64_t transport_failures = 0;
        std::uint64_t probes = 0;
        std::uint64_t margin_changes = 0;       // headroom ramp steps taken (PLAN §10.7)
    };
    Metrics metrics() const;
    CircuitBreaker::State breakerState() const { return breaker_.state(); }
    double drainRatePerSecond() const { return scheduler_.ratePerSecond(); }

    // --- pure time helpers, exposed for unit tests ---
    static std::string isoUtc(std::int64_t epoch_ms);
    static std::optional<std::int64_t> parseHttpDateMs(const std::string& date_header);
    // XUNI :55-:05 window as seen at the given (server) wall time.
    static XuniWindowState xuniWindowAt(std::int64_t server_epoch_ms);

private:
    void threadLoop_();
    void trackServerDate_(const TransportResult& r);
    void handleDifficultyBody_(const std::string& body);
    std::string backoffTimeIso_(std::int32_t attempt_count,
                                std::optional<long> retry_after_s) const;
    StepResult probeStep_();
    StepResult submitStep_();
    void logBreakerTransition_(CircuitBreaker::State before,
                               CircuitBreaker::State after,
                               const char* cause);
    StepResult confirmStep_();
    // Tracks the outage clock and re-evaluates the headroom ramp. Called at the top of every
    // step; does real work at most once per margin_eval_interval_ms.
    void updateMargin_();

    IFindJournal& journal_;
    ITransport& transport_;
    Config cfg_;
    MonotonicClock mono_;
    WallClock wall_;
    CircuitBreaker breaker_;
    DrainScheduler scheduler_;

    std::function<void(std::uint32_t)> difficulty_hint_cb_;
    std::optional<std::uint32_t> last_difficulty_;
    DifficultyTrend trend_ = DifficultyTrend::Unknown;
    std::optional<std::int64_t> server_offset_ms_;
    std::int64_t next_submit_allowed_ms_ = 0;
    bool last_window_open_ = false;

    // Margin state. margin_kib_ is atomic because the mine loop reads it on every batch.
    std::atomic<std::uint32_t> margin_kib_{0};
    std::function<void(std::uint32_t)> margin_cb_;   // guarded by state_mutex_
    std::atomic<std::int64_t> outage_started_ms_{0}; // 0 = /verify path is not open
    std::int64_t last_margin_eval_ms_ = 0;
    bool margin_eval_started_ = false;

    Metrics metrics_{};
    mutable std::mutex state_mutex_;  // guards callback/offset/metrics vs reader threads

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
};

}  // namespace treeminer
