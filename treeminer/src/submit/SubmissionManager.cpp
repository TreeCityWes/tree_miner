#include "SubmissionManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>

#include "../ConsoleLog.h"

namespace treeminer {

namespace {

constexpr std::int64_t kMsPerHour = 3600LL * 1000LL;
constexpr std::int64_t kXuniOpenBeforeHourMs = 5LL * 60LL * 1000LL;  // :55
constexpr std::int64_t kXuniOpenAfterHourMs = 5LL * 60LL * 1000LL;   // :05

std::int64_t defaultMonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t defaultWallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Howard Hinnant's civil-date algorithms (public domain formulation).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

int monthFromName(const std::string& mon) {
    static const char* names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (mon == names[i]) {
            return i + 1;
        }
    }
    return 0;
}

bool isBlankBody(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

const char* logKind(FindKind kind) {
    return kind == FindKind::XEN11 ? "XEN11" : "XUNI";
}

const char* logStatus(FindStatus status) {
    switch (status) {
        case FindStatus::Pending: return "Pending";
        case FindStatus::Submitting: return "Submitting";
        case FindStatus::AcceptedUnconfirmed: return "AcceptedUnconfirmed";
        case FindStatus::Acked: return "Acked";
        case FindStatus::ParkedDifficulty: return "ParkedDifficulty";
        case FindStatus::ParkedXuniWindow: return "ParkedXuniWindow";
        case FindStatus::Quarantined: return "Quarantined";
        case FindStatus::Dead: return "Dead";
        case FindStatus::PermanentlyInvalid: return "PermanentlyInvalid";
    }
    return "Unknown";
}

}  // namespace

// --- pure time helpers ---

std::string SubmissionManager::isoUtc(std::int64_t epoch_ms) {
    const std::int64_t days = floorDiv(epoch_ms, 86400000LL);
    std::int64_t rem = epoch_ms - days * 86400000LL;
    std::int64_t y;
    unsigned mo, da;
    civilFromDays(days, y, mo, da);
    const int h = static_cast<int>(rem / 3600000LL);
    rem %= 3600000LL;
    const int mi = static_cast<int>(rem / 60000LL);
    rem %= 60000LL;
    const int s = static_cast<int>(rem / 1000LL);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02dZ",
                  static_cast<long long>(y), mo, da, h, mi, s);
    return std::string(buf);
}

std::optional<std::int64_t> SubmissionManager::parseHttpDateMs(const std::string& date_header) {
    // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    std::string rest = date_header;
    const std::size_t comma = rest.find(',');
    if (comma != std::string::npos) {
        rest = rest.substr(comma + 1);
    }
    std::istringstream in(rest);
    int day = 0, year = 0;
    std::string mon, hms, tz;
    if (!(in >> day >> mon >> year >> hms >> tz)) {
        return std::nullopt;
    }
    if (tz != "GMT" && tz != "UTC") {
        return std::nullopt;
    }
    const int month = monthFromName(mon);
    if (month == 0 || day < 1 || day > 31 || year < 1970) {
        return std::nullopt;
    }
    if (hms.size() != 8 || hms[2] != ':' || hms[5] != ':') {
        return std::nullopt;
    }
    for (std::size_t i : {0u, 1u, 3u, 4u, 6u, 7u}) {
        if (!std::isdigit(static_cast<unsigned char>(hms[i]))) {
            return std::nullopt;
        }
    }
    const int h = (hms[0] - '0') * 10 + (hms[1] - '0');
    const int mi = (hms[3] - '0') * 10 + (hms[4] - '0');
    const int s = (hms[6] - '0') * 10 + (hms[7] - '0');
    if (h > 23 || mi > 59 || s > 60) {
        return std::nullopt;
    }
    const std::int64_t days = daysFromCivil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
    return days * 86400000LL + (static_cast<std::int64_t>(h) * 3600LL +
                                mi * 60LL + s) * 1000LL;
}

XuniWindowState SubmissionManager::xuniWindowAt(std::int64_t server_epoch_ms) {
    XuniWindowState w;
    std::int64_t into_hour = server_epoch_ms - floorDiv(server_epoch_ms, kMsPerHour) * kMsPerHour;
    if (into_hour >= kMsPerHour - kXuniOpenBeforeHourMs) {
        // :55 .. :59 — closes at :05 past the NEXT hour.
        w.open = true;
        w.ms_until_close = (kMsPerHour - into_hour) + kXuniOpenAfterHourMs;
    } else if (into_hour < kXuniOpenAfterHourMs) {
        // :00 .. :04 — closes at :05.
        w.open = true;
        w.ms_until_close = kXuniOpenAfterHourMs - into_hour;
    }
    return w;
}

// --- construction / threading ---

SubmissionManager::SubmissionManager(IFindJournal& journal, ITransport& transport)
    : SubmissionManager(journal, transport, Config{}) {}

SubmissionManager::SubmissionManager(IFindJournal& journal, ITransport& transport, Config cfg,
                                     MonotonicClock monotonic, WallClock wall)
    : journal_(journal),
      transport_(transport),
      cfg_(cfg),
      mono_(monotonic ? std::move(monotonic) : MonotonicClock(defaultMonotonicMs)),
      wall_(wall ? std::move(wall) : WallClock(defaultWallMs)),
      breaker_(cfg.breaker, [this] { return mono_(); }),
      scheduler_(cfg.drain) {}

SubmissionManager::~SubmissionManager() {
    stop();
}

void SubmissionManager::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] { threadLoop_(); });
}

void SubmissionManager::stop() {
    // No early-out on "already not running": after a fatal exception (finding 8) the
    // loop clears running_ itself, but the std::thread object is still joinable — an
    // exchange-guarded early return here would skip the join and let the destructor
    // hit std::terminate on a joinable thread. Always fall through to the join.
    running_.store(false);
    {
        std::lock_guard<std::mutex> lk(wake_mutex_);
    }
    wake_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SubmissionManager::notifyFindAppended() {
    wake_cv_.notify_all();
}

void SubmissionManager::threadLoop_() {
    // runOnce() is the exception boundary (finding 8) and never throws; the belt-and-
    // braces try/catch here covers the residual loop machinery (lock/wait) so that NO
    // path out of this thread function can reach std::terminate.
    try {
        while (running_.load()) {
            StepResult r = StepResult::Idle;
            r = runOnce();
            std::unique_lock<std::mutex> lk(wake_mutex_);
            const auto wait_ms = (r == StepResult::Idle) ? cfg_.idle_poll_ms
                                                         : std::min<std::int64_t>(cfg_.idle_poll_ms, 50);
            wake_cv_.wait_for(lk, std::chrono::milliseconds(wait_ms),
                              [this] { return !running_.load(); });
        }
    } catch (const std::exception& e) {
        handleFatal_(std::string("submission thread loop: ") + e.what());
    } catch (...) {
        handleFatal_("submission thread loop: unknown exception");
    }
}

void SubmissionManager::handleFatal_(const std::string& what) {
    if (fatal_.exchange(true)) {
        return;  // first exception wins; later ones (there should be none) are dropped
    }
    // A submission layer that cannot touch its journal must not spin: every further step
    // would fail the same way while looking "alive" to the operator. Stop the loop; the
    // thread exits cleanly and remains joinable (stop() still works, see above).
    running_.store(false);
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        ++metrics_.thread_loop_exceptions;
    }
    ConsoleLog::event(ConsoleLog::Level::Error, "SUBMIT",
                      "FATAL | submission loop halted — journal or step failure, finds are "
                      "still durable on disk but will NOT drain until restart | " + what);
    FatalCallback cb;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        cb = fatal_cb_;
    }
    if (cb) {
        cb(what);
    }
    // Wake any waiter promptly (e.g. runOnce driven from another thread while the loop
    // thread sits in wait_for) so the halt is observed without an idle-poll delay.
    wake_cv_.notify_all();
}

// --- difficulty + server clock ---

void SubmissionManager::setDifficultyHintCallback(std::function<void(std::uint32_t)> cb) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    difficulty_hint_cb_ = std::move(cb);
}

void SubmissionManager::setOutcomeCallback(OutcomeCallback cb) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    outcome_cb_ = std::move(cb);
}

void SubmissionManager::setNetworkStateCallback(NetworkStateCallback cb) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    network_state_cb_ = std::move(cb);
}

void SubmissionManager::setFatalCallback(FatalCallback cb) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    fatal_cb_ = std::move(cb);
}

void SubmissionManager::emitOutcome_(const FindRecord& record,
                                     const Classification& classification,
                                     std::optional<int> http_status) {
    OutcomeCallback callback;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        callback = outcome_cb_;
    }
    if (callback) {
        callback(record, classification, http_status);
    }
}

void SubmissionManager::emitNetworkState_() {
    NetworkStateCallback callback;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        callback = network_state_cb_;
    }
    if (callback) {
        callback(breaker_.state());
    }
}

void SubmissionManager::observeDifficulty(std::uint32_t difficulty) {
    bool decreased = false;
    bool first_observation = false;
    std::optional<std::uint32_t> previous;
    DifficultyTrend new_trend = DifficultyTrend::Unknown;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        previous = last_difficulty_;
        if (last_difficulty_) {
            if (difficulty > *last_difficulty_) {
                trend_ = DifficultyTrend::Rising;
            } else if (difficulty < *last_difficulty_) {
                trend_ = DifficultyTrend::Falling;
                decreased = true;
            } else {
                trend_ = DifficultyTrend::Flat;
            }
        } else {
            first_observation = true;
        }
        last_difficulty_ = difficulty;
        new_trend = trend_;
    }
    std::size_t unparked = 0;
    // PLAN §3.3(b): a falling floor re-qualifies parked finds with m >= current.
    //
    // The first observation of a process must un-park too, even though there is no trend to
    // compare against. A restart begins with last_difficulty_ empty, so gating purely on a
    // strict decrease left finds parked that were already valid again — they would wait for
    // some LATER decrease that may never come while difficulty trends upward. Recovering
    // them is the whole point of parking rather than dropping (PLAN §3.4). The UPDATE is
    // bounded to ParkedDifficulty rows with m >= current, so a no-op costs nothing.
    if (decreased || first_observation) {
        unparked = journal_.unparkForDifficulty(difficulty);
    }
    if (!previous || *previous != difficulty) {
        const char* trend = new_trend == DifficultyTrend::Rising ? "rising"
                            : new_trend == DifficultyTrend::Falling ? "falling"
                            : new_trend == DifficultyTrend::Flat ? "flat"
                                                                 : "unknown";
        std::ostringstream message;
        message << "previous=";
        if (previous) {
            message << *previous;
        } else {
            message << "unknown";
        }
        message << " -> current=" << difficulty
                << " | trend=" << trend
                << " | unparked=" << unparked;
        ConsoleLog::event(decreased ? ConsoleLog::Level::Info : ConsoleLog::Level::Warn,
                          "DIFFICULTY", message.str());
    }
}

std::optional<std::uint32_t> SubmissionManager::lastObservedDifficulty() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return last_difficulty_;
}

std::optional<std::int64_t> SubmissionManager::serverClockOffsetMs() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return server_offset_ms_;
}

SubmissionManager::Metrics SubmissionManager::metrics() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    return metrics_;
}

// --- difficulty margin (PLAN §5, §10.7) ---

void SubmissionManager::setMarginCallback(std::function<void(std::uint32_t)> cb) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    margin_cb_ = std::move(cb);
}

std::int64_t SubmissionManager::outageDurationMs() const {
    const std::int64_t started = outage_started_ms_.load();
    if (started == 0) {
        return 0;
    }
    const std::int64_t elapsed = mono_() - started;
    return elapsed > 0 ? elapsed : 0;
}

void SubmissionManager::updateMargin_() {
    const std::int64_t now = mono_();

    // Outage clock: the /verify path being OPEN is what puts finds at risk. Tracked here
    // rather than in CircuitBreaker so the breaker stays a pure state machine.
    const bool open = breaker_.state() == CircuitBreaker::State::Open;
    if (open) {
        if (outage_started_ms_.load() == 0) {
            outage_started_ms_.store(now);
        }
    } else {
        // Latch the outage span before clearing the live clock, so the RECOVERED log (which
        // fires a step later, once the breaker fully closes) can report how long we were down.
        const std::int64_t started = outage_started_ms_.load();
        if (started != 0) {
            const std::int64_t span = now - started;
            last_outage_span_ms_.store(span > 0 ? span : 0);
        }
        outage_started_ms_.store(0);
    }

    if (cfg_.margin.mode == MarginMode::Off) {
        return;  // margin_kib_ stays 0; never touch the mine loop when the feature is off
    }

    // Rate-limit: Auto reads journal counts, and the ramp moves on a 300 s scale anyway.
    if (margin_eval_started_ && (now - last_margin_eval_ms_) < cfg_.margin_eval_interval_ms) {
        return;
    }
    last_margin_eval_ms_ = now;
    margin_eval_started_ = true;

    MarginInputs in;
    in.breaker_open = open;
    in.outage_ms = open ? (now - outage_started_ms_.load()) : 0;
    if (cfg_.margin.mode == MarginMode::Auto) {
        // Backlog = everything journaled that has not reached a terminal state. Parked and
        // unconfirmed records count: they are finds we still owe the operator.
        const IFindJournal::Counts c = journal_.counts();
        in.backlog = c.pending + c.parked + c.accepted_unconfirmed + c.quarantined;
    }

    const std::uint32_t next = computeMargin(cfg_.margin, in);
    if (next == margin_kib_.load()) {
        return;
    }
    margin_kib_.store(next);
    std::function<void(std::uint32_t)> cb;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        ++metrics_.margin_changes;
        cb = margin_cb_;
    }
    if (cb) {
        cb(next);
    }
}

void SubmissionManager::trackServerDate_(const TransportResult& r) {
    if (!r.transport_ok || !r.date_header) {
        return;
    }
    if (auto server_ms = parseHttpDateMs(*r.date_header)) {
        std::lock_guard<std::mutex> lk(state_mutex_);
        server_offset_ms_ = *server_ms - wall_();
    }
}

void SubmissionManager::handleDifficultyBody_(const std::string& body) {
    // gpage.py:115 — {"difficulty": "<N>"} (a JSON string).
    auto field = extractJsonField(body, "difficulty");
    if (!field) {
        return;
    }
    if (auto d = parseDifficultyHint("m=" + *field)) {  // reuse the bounded digit parser
        observeDifficulty(*d);
        journal_.recordDifficulty(*d, isoUtc(wall_()));
        std::function<void(std::uint32_t)> cb;
        {
            std::lock_guard<std::mutex> lk(state_mutex_);
            cb = difficulty_hint_cb_;
        }
        if (cb) {
            cb(*d);
        }
    }
}

std::string SubmissionManager::backoffTimeIso_(std::int32_t attempt_count,
                                               std::optional<long> retry_after_s) const {
    std::int64_t delay = cfg_.backoff_base_ms;
    if (retry_after_s) {
        delay = static_cast<std::int64_t>(*retry_after_s) * 1000LL;
    } else {
        for (std::int32_t i = 0; i < attempt_count && delay < cfg_.backoff_cap_ms; ++i) {
            delay *= 2;
        }
    }
    delay = std::min(delay, cfg_.backoff_cap_ms);
    return isoUtc(wall_() + delay);
}

// --- confirmation body validation (finding 4) ---

SubmissionManager::ConfirmBodyCheck SubmissionManager::confirmationMatches(
        const FindRecord& record, const std::string& body) {
    // gpage.py:331-364 — a /get_block 200 body IS the stored row:
    //   {block_id, hash_to_verify, key, account, created_at}
    // Over plaintext HTTP a captive portal / transparent proxy / MITM can answer 200 to
    // anything; treating transport-ok + 200 as proof would let it permanently suppress
    // resubmission of real finds. So the body must positively identify OUR row:
    //   1. it parses as a JSON object with a scalar "key", and
    //   2. that key is byte-equal to the record's key (keys are lowercase 64-hex we
    //      generated ourselves and echo verbatim, so byte equality is the right test —
    //      any normalization here would only widen what an attacker may return), and
    //   3. if the row carries hash_to_verify, it is byte-equal to the record's immutable
    //      hash (the key is derived from the hash, so a mismatch means the server holds
    //      a DIFFERENT find under our key — credit theft or corruption, never ackable).
    auto key = extractJsonField(body, "key");
    if (!key) {
        return ConfirmBodyCheck::Malformed;
    }
    if (*key != record.payload.key) {
        return ConfirmBodyCheck::KeyMismatch;
    }
    if (auto hash = extractJsonField(body, "hash_to_verify")) {
        if (*hash != record.payload.hash_to_verify) {
            return ConfirmBodyCheck::HashMismatch;
        }
    }
    return ConfirmBodyCheck::Confirmed;
}

namespace {

// One reason fragment per rejected-body case, appended to the record's status_reason so
// the journal shows WHY a 200 was not trusted. All three outcomes keep the record
// AcceptedUnconfirmed with the normal backoff: never Acked (nothing was proven), never
// Pending (only a real 404 proves absence — the server may genuinely hold the row).
const char* confirmRejectReason(SubmissionManager::ConfirmBodyCheck check) {
    switch (check) {
        case SubmissionManager::ConfirmBodyCheck::KeyMismatch:
            return "/get_block 200 body describes a different key — not a confirmation "
                   "of this record, remaining unconfirmed";
        case SubmissionManager::ConfirmBodyCheck::HashMismatch:
            return "/get_block 200 has our key but a DIFFERENT hash_to_verify — refusing "
                   "to ack, remaining unconfirmed";
        default:
            return "/get_block 200 body malformed or missing key — untrusted, remaining "
                   "unconfirmed";
    }
}

}  // namespace

// --- the scheduling step ---

SubmissionManager::StepResult SubmissionManager::runOnce() {
    // Finding 8: this is the exception boundary between the journal/transport machinery
    // and the thread function. A JournalError from recordAttempt/counts/recordDifficulty/
    // unpark used to escape threadLoop_ and std::terminate the whole miner; now the first
    // exception halts the drain loop cleanly (handleFatal_) and later calls are inert.
    if (fatal_.load()) {
        return StepResult::Idle;
    }
    try {
        return runStep_();
    } catch (const std::exception& e) {
        handleFatal_(std::string("submission step: ") + e.what());
    } catch (...) {
        handleFatal_("submission step: unknown exception");
    }
    return StepResult::Idle;
}

SubmissionManager::StepResult SubmissionManager::runStep_() {
    // Headroom is re-evaluated first so the outage clock advances even on steps that do no
    // network work at all (an OPEN breaker whose probe is not yet due still ages the outage).
    updateMargin_();
    if (breaker_.state() == CircuitBreaker::State::Open) {
        // OPEN: probes only — no /verify traffic and no confirmation lookups either
        // (they target the same host; hammering /get_block during an outage helps nobody).
        return probeStep_();
    }
    const StepResult submit_result = submitStep_();
    // Confirmation retries run after the normal drain step and outside the drain-rate
    // budget, so a backlog of unconfirmed acks can never starve fresh submissions.
    const StepResult confirm_result = confirmStep_();
    if (submit_result == StepResult::Idle && confirm_result != StepResult::Idle) {
        return confirm_result;
    }
    return submit_result;
}

SubmissionManager::StepResult SubmissionManager::probeStep_() {
    if (!breaker_.probeDue()) {
        return StepResult::Idle;
    }
    TransportResult r = transport_.difficulty();
    trackServerDate_(r);
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        ++metrics_.probes;
    }
    if (r.transport_ok && r.http_status == 200 && !isBlankBody(r.body)) {
        handleDifficultyBody_(r.body);
        const auto before = breaker_.state();
        breaker_.onProbeSuccess();  // HALF_OPEN: next step admits one real submission
        logBreakerTransition_(before, breaker_.state(), "difficulty probe succeeded");
    } else {
        breaker_.onProbeFailure();
    }
    emitNetworkState_();
    return StepResult::Probed;
}

SubmissionManager::StepResult SubmissionManager::submitStep_() {
    const std::int64_t now_mono = mono_();
    if (now_mono < next_submit_allowed_ms_) {
        return StepResult::Idle;
    }

    // XUNI window, on the server's clock when we know the offset.
    std::int64_t offset = 0;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        offset = server_offset_ms_.value_or(0);
    }
    const XuniWindowState window = xuniWindowAt(wall_() + offset);
    if (window.open && !last_window_open_) {
        // A window just opened: parked XUNI with remaining budget become Pending again.
        journal_.unparkXuniForWindow(cfg_.xuni_max_windows);
    }
    last_window_open_ = window.open;

    // Fetch per kind rather than taking one mixed oldest-first slice. A single LIMITed slice
    // lets either kind starve the other, and both directions are reachable in normal
    // operation:
    //   * XUNI is journaled Pending whenever it is found, including outside a window, but is
    //     not submittable then. `fetch_limit` such records ahead of a XEN11 backlog produced
    //     a slice with nothing selectable in it — the drain reported Idle and NO XEN11 was
    //     submitted until the next :55 window, against a perfectly healthy server.
    //   * Symmetrically, a XEN11 backlog deeper than `fetch_limit` could hide a XUNI whose
    //     window is closing — the one record that genuinely cannot wait.
    // Asking per kind lets DrainScheduler apply its priority rules (XEN11 first; XUNI
    // preempts near the window end) to what actually exists, not to whatever the first
    // `limit` rows happened to be. (Merge note: this supersedes the scan-whole-queue fix
    // for the same bug — that variant hydrated the entire Pending backlog every step,
    // O(backlog) per 250 ms; the per-kind fetch stays two indexed LIMIT queries.)
    const std::string now_iso = isoUtc(wall_());
    std::vector<FindRecord> eligible =
        journal_.fetchEligibleOfKind(FindKind::XEN11, now_iso, cfg_.fetch_limit);
    bool xuni_pressure = false;
    if (window.open) {
        // Only worth asking while the window is open: outside it, XUNI is never selectable.
        std::vector<FindRecord> xuni =
            journal_.fetchEligibleOfKind(FindKind::XUNI, now_iso, cfg_.fetch_limit);
        xuni_pressure = !xuni.empty();
        eligible.insert(eligible.end(), xuni.begin(), xuni.end());
    }
    breaker_.setXuniPressure(xuni_pressure);
    if (eligible.empty()) {
        return StepResult::Idle;
    }

    const FindRecord* rec = scheduler_.selectNext(eligible, difficultyTrend(), window);
    if (rec == nullptr) {
        return StepResult::Idle;
    }
    if (!breaker_.tryAdmit()) {
        return StepResult::BreakerBlocked;
    }

    const bool was_closed = breaker_.state() == CircuitBreaker::State::Closed;

    TransportResult res = transport_.submit(rec->payload);
    trackServerDate_(res);
    const int status = res.transport_ok ? res.http_status : kTransportError;
    Classification c = classify(status, res.body, rec->payload.kind, res.retry_after);
    const std::optional<std::uint32_t> known_difficulty_before_response =
        lastObservedDifficulty();

    // Difficulty hint from a 401 body: update the cache without waiting for the poller.
    if (c.server_difficulty_hint) {
        observeDifficulty(*c.server_difficulty_hint);
        journal_.recordDifficulty(*c.server_difficulty_hint, isoUtc(wall_()));
        std::function<void(std::uint32_t)> cb;
        {
            std::lock_guard<std::mutex> lk(state_mutex_);
            cb = difficulty_hint_cb_;
        }
        if (cb) {
            cb(*c.server_difficulty_hint);
        }
    }

    // Confirmation-aware acks: 200 and duplicates are only Acked once /get_block agrees —
    // and "agrees" means the 200 BODY describes this record (finding 4), not merely that
    // some intermediary answered 200 over plaintext HTTP.
    std::optional<std::string> next_attempt;
    if (c.needs_lookup_confirmation) {
        TransportResult conf = transport_.confirm(rec->payload.key);
        trackServerDate_(conf);
        const ConfirmBodyCheck body_check =
            (conf.transport_ok && conf.http_status == 200)
                ? confirmationMatches(*rec, conf.body)
                : ConfirmBodyCheck::Malformed;  // unused unless status == 200
        if (conf.transport_ok && conf.http_status == 200 &&
            body_check == ConfirmBodyCheck::Confirmed) {
            c.next_status = FindStatus::Acked;
            c.needs_lookup_confirmation = false;
            c.reason += "; confirmed via /get_block (body matches key)";
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.reconciled_via_get_block;
        } else if (conf.transport_ok && conf.http_status == 200) {
            // A 200 whose body does not prove our row: stay AcceptedUnconfirmed with the
            // normal backoff and let the confirmStep_ retry re-ask later. A hash mismatch
            // is the serious flavor — the server (or something between us and it) holds a
            // different find under our key — so it is additionally logged at Error.
            if (body_check == ConfirmBodyCheck::HashMismatch) {
                ConsoleLog::event(ConsoleLog::Level::Error, "CONFIRM",
                                  "hash_to_verify MISMATCH on /get_block for key=" +
                                  rec->payload.key +
                                  " — server row differs from our immutable find; NOT "
                                  "acking (possible interception or server corruption)");
            }
            c.reason += std::string("; ") + confirmRejectReason(body_check);
            next_attempt = backoffTimeIso_(rec->attempt_count, std::nullopt);
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.confirm_body_rejected;
        } else if (conf.transport_ok && conf.http_status == 404) {
            // The lying-200 (gpage.py:492-494,515): the server said saved, the lookup says
            // absent. Resubmit — replay is idempotent thanks to the UNIQUE key.
            c.next_status = FindStatus::Pending;
            c.needs_lookup_confirmation = false;
            c.reason += "; /get_block says ABSENT — server 200 was not durable, resubmitting";
            next_attempt = backoffTimeIso_(rec->attempt_count, std::nullopt);
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.lying_200_detected;
        } else {
            // Lookup unavailable: remain AcceptedUnconfirmed with a backed-off
            // next_attempt_at so fetchAwaitingConfirmation re-drives it later
            // (contract v1.1) — never presented as confirmed (PLAN §10.2).
            c.reason += "; /get_block unavailable, remaining unconfirmed";
            next_attempt = backoffTimeIso_(rec->attempt_count, std::nullopt);
        }
    }

    std::ostringstream submission_message;
    submission_message << logKind(rec->payload.kind) << " #" << rec->id;
    if (c.next_status == FindStatus::Acked) {
        submission_message << " confirmed";
    } else if (c.next_status == FindStatus::Pending) {
        submission_message << " retry " << (rec->attempt_count + 1);
    } else {
        submission_message << " " << logStatus(c.next_status);
    }
    submission_message << " | m=" << rec->payload.memory_cost;
    // Margin vs the server difficulty is the most useful field when a find is parked or
    // rejected: it answers "did I mine enough headroom?". Prefer the fresh hint from this
    // response body, else the last difficulty we observed.
    const std::optional<std::uint32_t> server_m =
        c.server_difficulty_hint ? c.server_difficulty_hint : known_difficulty_before_response;
    if (server_m) {
        submission_message << " vs server " << *server_m << " (margin " << std::showpos
                           << (static_cast<std::int64_t>(rec->payload.memory_cost) -
                               static_cast<std::int64_t>(*server_m))
                           << std::noshowpos << ")";
    }
    submission_message << " | ";
    if (res.transport_ok) {
        submission_message << "HTTP " << res.http_status;
    } else {
        submission_message << "network unavailable";
    }
    // Keep the reason ("why parked/rejected/resubmitting") on the console for anything that
    // is not a clean confirmation — this is the thread an operator pulls during an outage.
    if (c.next_status != FindStatus::Acked && !c.reason.empty()) {
        submission_message << " | " << c.reason;
    }
    const ConsoleLog::Level submission_level =
        c.next_status == FindStatus::Acked ? ConsoleLog::Level::Ok
        : c.next_status == FindStatus::ParkedDifficulty ||
          c.next_status == FindStatus::ParkedXuniWindow ? ConsoleLog::Level::Park
        : c.next_status == FindStatus::Pending ? ConsoleLog::Level::Retry
        : c.next_status == FindStatus::PermanentlyInvalid ||
          c.next_status == FindStatus::Quarantined ? ConsoleLog::Level::Error
                                                    : ConsoleLog::Level::Info;
    ConsoleLog::event(submission_level, "SUBMIT", submission_message.str());

    if (c.next_status == FindStatus::Pending && !next_attempt) {
        std::optional<long> retry_after_s;
        if (status == 429 && res.retry_after) {
            retry_after_s = parseRetryAfterSeconds(*res.retry_after);
        }
        next_attempt = backoffTimeIso_(rec->attempt_count, retry_after_s);
    }

    const std::optional<int> http_status =
        res.transport_ok ? std::optional<int>(res.http_status) : std::nullopt;
    journal_.recordAttempt(rec->id, c, http_status, res.body, next_attempt, isoUtc(wall_()));
    emitOutcome_(*rec, c, http_status);

    // Breaker + adaptive pacing.
    const bool transport_failure = !res.transport_ok || res.http_status >= 500 ||
                                   res.http_status == 408 || res.http_status == 425 ||
                                   isBlankBody(res.body);
    const bool accepted = c.next_status == FindStatus::Acked ||
                          c.next_status == FindStatus::AcceptedUnconfirmed;
    const auto breaker_before_outcome = breaker_.state();
    if (accepted) {
        breaker_.onVerifySuccess();
        if (was_closed) {
            scheduler_.onHealthyRoundTrip();
        } else {
            scheduler_.onBreakerClose();  // recovery drain restarts at 1/s
        }
    } else if (transport_failure) {
        breaker_.onVerifyTransportFailure();
        scheduler_.onThrottle();
    } else if (res.http_status == 429) {
        breaker_.onVerifyInconclusive();
        scheduler_.onThrottle();
    } else {
        // Conclusive non-success (parked/quarantined/invalid): the round-trip itself
        // was healthy.
        breaker_.onVerifyInconclusive();
        scheduler_.onHealthyRoundTrip();
    }
    logBreakerTransition_(breaker_before_outcome, breaker_.state(),
                          transport_failure ? "verification transport failure"
                                            : "verification response");
    emitNetworkState_();

    next_submit_allowed_ms_ = now_mono + scheduler_.submitIntervalMs();

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        ++metrics_.submitted;
        if (rec->attempt_count > 0) {
            ++metrics_.resubmitted;
        }
        if (!res.transport_ok) {
            ++metrics_.transport_failures;
        }
        switch (c.next_status) {
            case FindStatus::Acked: ++metrics_.acked; break;
            case FindStatus::AcceptedUnconfirmed: ++metrics_.accepted_unconfirmed; break;
            case FindStatus::ParkedDifficulty: ++metrics_.parked_difficulty; break;
            case FindStatus::ParkedXuniWindow: ++metrics_.parked_xuni; break;
            case FindStatus::Quarantined: ++metrics_.quarantined; break;
            case FindStatus::PermanentlyInvalid: ++metrics_.permanently_invalid; break;
            default: break;
        }
    }
    return StepResult::Submitted;
}

void SubmissionManager::logBreakerTransition_(CircuitBreaker::State before,
                                               CircuitBreaker::State after,
                                               const char* cause) {
    if (before == after) {
        return;
    }

    const IFindJournal::Counts counts = journal_.counts();
    const std::size_t backlog = counts.pending + counts.parked +
                                counts.accepted_unconfirmed + counts.quarantined;
    auto human_ms = [](std::int64_t ms) {
        const std::int64_t s = ms / 1000;
        std::ostringstream o;
        if (s >= 60) o << (s / 60) << "m " << (s % 60) << "s";
        else o << s << "s";
        return o.str();
    };
    std::ostringstream message;
    if (after == CircuitBreaker::State::Open) {
        const auto retry_ms = std::max<std::int64_t>(0, breaker_.nextProbeAtMs() - mono_());
        message << "submissions paused (" << cause << ") — " << backlog << " find"
                << (backlog == 1 ? "" : "s") << " safe in queue; retry in "
                << (retry_ms / 1000) << "s";
        ConsoleLog::event(ConsoleLog::Level::Warn, "NETWORK", message.str());
    } else if (after == CircuitBreaker::State::HalfOpen) {
        return;
    } else {
        // Outage duration is the headline recovery metric — "how long were we down" — and
        // is otherwise unrecoverable once the breaker closes. Read the latched span, not the
        // live clock: updateMargin_ zeroes outage_started_ms_ before this transition fires.
        message << "submissions restored after " << human_ms(last_outage_span_ms_.load())
                << " — " << backlog << " queued, draining";
        ConsoleLog::event(ConsoleLog::Level::Ok, "NETWORK", message.str());
    }
}

SubmissionManager::StepResult SubmissionManager::confirmStep_() {
    // Re-drive AcceptedUnconfirmed rows whose confirmation lookup previously failed
    // (contract v1.1: fetchAwaitingConfirmation honors the persisted next_attempt_at).
    // Skipped while the breaker is OPEN — including when this very step opened it.
    if (breaker_.state() == CircuitBreaker::State::Open) {
        return StepResult::Idle;
    }
    std::vector<FindRecord> batch =
        journal_.fetchAwaitingConfirmation(isoUtc(wall_()), cfg_.confirm_fetch_limit);
    if (batch.empty()) {
        return StepResult::Idle;
    }
    bool any = false;
    for (const FindRecord& rec : batch) {
        TransportResult conf = transport_.confirm(rec.payload.key);
        trackServerDate_(conf);

        Classification c;
        std::optional<std::string> next_attempt;
        const ConfirmBodyCheck body_check =
            (conf.transport_ok && conf.http_status == 200)
                ? confirmationMatches(rec, conf.body)
                : ConfirmBodyCheck::Malformed;  // unused unless status == 200
        if (conf.transport_ok && conf.http_status == 200 &&
            body_check == ConfirmBodyCheck::Confirmed) {
            c.next_status = FindStatus::Acked;
            c.reason = "confirmed via /get_block (retry, body matches key)";
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.reconciled_via_get_block;
        } else if (conf.transport_ok && conf.http_status == 200) {
            // Same rule as the initial confirmation (finding 4): an unproven 200 keeps
            // the record AcceptedUnconfirmed with per-record backoff — never Acked,
            // never demoted to Pending.
            if (body_check == ConfirmBodyCheck::HashMismatch) {
                ConsoleLog::event(ConsoleLog::Level::Error, "CONFIRM",
                                  "hash_to_verify MISMATCH on /get_block for key=" +
                                  rec.payload.key +
                                  " — server row differs from our immutable find; NOT "
                                  "acking (possible interception or server corruption)");
            }
            c.next_status = FindStatus::AcceptedUnconfirmed;
            c.reason = confirmRejectReason(body_check);
            next_attempt = backoffTimeIso_(rec.attempt_count, std::nullopt);
            {
                std::lock_guard<std::mutex> lk(state_mutex_);
                ++metrics_.confirm_body_rejected;
            }
        } else if (conf.transport_ok && conf.http_status == 404) {
            // The lying-200, caught on retry: the row never became durable server-side.
            c.next_status = FindStatus::Pending;
            c.reason = "/get_block says ABSENT — server 200 was not durable, resubmitting";
            next_attempt = backoffTimeIso_(rec.attempt_count, std::nullopt);
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.lying_200_detected;
        } else {
            // Still unavailable (transport failure, 5xx, unexpected schema): stay
            // AcceptedUnconfirmed and push the retry out with per-record backoff.
            c.next_status = FindStatus::AcceptedUnconfirmed;
            c.reason = "/get_block unavailable, remaining unconfirmed (retry backoff)";
            next_attempt = backoffTimeIso_(rec.attempt_count, std::nullopt);
        }

        const std::optional<int> http_status =
            conf.transport_ok ? std::optional<int>(conf.http_status) : std::nullopt;
        journal_.recordAttempt(rec.id, c, http_status, conf.body, next_attempt, isoUtc(wall_()));
        std::ostringstream confirmation_message;
        confirmation_message << "id=" << rec.id
                             << " | attempt=" << (rec.attempt_count + 1)
                             << " | mined_m=" << rec.payload.memory_cost
                             << " | http=";
        if (conf.transport_ok) {
            confirmation_message << conf.http_status;
        } else {
            confirmation_message << "transport_error";
        }
        confirmation_message << " | " << logStatus(c.next_status)
                             << " | " << c.reason;
        ConsoleLog::event(c.next_status == FindStatus::Acked ? ConsoleLog::Level::Ok
                                                               : ConsoleLog::Level::Retry,
                          "CONFIRM", confirmation_message.str());
        emitOutcome_(rec, c, http_status);
        {
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.confirmation_retries;
        }
        any = true;
        if (!conf.transport_ok) {
            break;  // the host looks down — don't hammer it with the rest of the batch
        }
    }
    return any ? StepResult::ConfirmRetried : StepResult::Idle;
}

}  // namespace treeminer
