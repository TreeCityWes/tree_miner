#include "SubmissionManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>

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
    if (!running_.exchange(false)) {
        return;
    }
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
    while (running_.load()) {
        StepResult r = StepResult::Idle;
        r = runOnce();
        std::unique_lock<std::mutex> lk(wake_mutex_);
        const auto wait_ms = (r == StepResult::Idle) ? cfg_.idle_poll_ms
                                                     : std::min<std::int64_t>(cfg_.idle_poll_ms, 50);
        wake_cv_.wait_for(lk, std::chrono::milliseconds(wait_ms),
                          [this] { return !running_.load(); });
    }
}

// --- difficulty + server clock ---

void SubmissionManager::setDifficultyHintCallback(std::function<void(std::uint32_t)> cb) {
    std::lock_guard<std::mutex> lk(state_mutex_);
    difficulty_hint_cb_ = std::move(cb);
}

void SubmissionManager::observeDifficulty(std::uint32_t difficulty) {
    bool decreased = false;
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (last_difficulty_) {
            if (difficulty > *last_difficulty_) {
                trend_ = DifficultyTrend::Rising;
            } else if (difficulty < *last_difficulty_) {
                trend_ = DifficultyTrend::Falling;
                decreased = true;
            } else {
                trend_ = DifficultyTrend::Flat;
            }
        }
        last_difficulty_ = difficulty;
    }
    if (decreased) {
        // PLAN §3.3(b): a falling floor re-qualifies parked finds with m >= current.
        journal_.unparkForDifficulty(difficulty);
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

// --- the scheduling step ---

SubmissionManager::StepResult SubmissionManager::runOnce() {
    if (breaker_.state() == CircuitBreaker::State::Open) {
        return probeStep_();
    }
    return submitStep_();
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
        breaker_.onProbeSuccess();  // HALF_OPEN: next step admits one real submission
    } else {
        breaker_.onProbeFailure();
    }
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

    std::vector<FindRecord> eligible = journal_.fetchEligible(isoUtc(wall_()), cfg_.fetch_limit);
    bool xuni_pressure = false;
    for (const FindRecord& r : eligible) {
        if (r.payload.kind == FindKind::XUNI) {
            xuni_pressure = true;
            break;
        }
    }
    breaker_.setXuniPressure(xuni_pressure && window.open);
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

    // Confirmation-aware acks: 200 and duplicates are only Acked once /get_block agrees.
    std::optional<std::string> next_attempt;
    if (c.needs_lookup_confirmation) {
        TransportResult conf = transport_.confirm(rec->payload.key);
        trackServerDate_(conf);
        if (conf.transport_ok && conf.http_status == 200) {
            c.next_status = FindStatus::Acked;
            c.needs_lookup_confirmation = false;
            c.reason += "; confirmed via /get_block";
            std::lock_guard<std::mutex> lk(state_mutex_);
            ++metrics_.reconciled_via_get_block;
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
            // Lookup unavailable: remain AcceptedUnconfirmed, with metrics — never
            // presented as confirmed (PLAN §10.2).
            c.reason += "; /get_block unavailable, remaining unconfirmed";
        }
    }

    if (c.next_status == FindStatus::Pending && !next_attempt) {
        std::optional<long> retry_after_s;
        if (status == 429 && res.retry_after) {
            retry_after_s = parseRetryAfterSeconds(*res.retry_after);
        }
        next_attempt = backoffTimeIso_(rec->attempt_count, retry_after_s);
    }

    journal_.recordAttempt(rec->id, c,
                           res.transport_ok ? std::optional<int>(res.http_status) : std::nullopt,
                           res.body, next_attempt, isoUtc(wall_()));

    // Breaker + adaptive pacing.
    const bool transport_failure = !res.transport_ok || res.http_status >= 500 ||
                                   res.http_status == 408 || res.http_status == 425 ||
                                   isBlankBody(res.body);
    const bool accepted = c.next_status == FindStatus::Acked ||
                          c.next_status == FindStatus::AcceptedUnconfirmed;
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

    next_submit_allowed_ms_ = now_mono + scheduler_.submitIntervalMs();

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        ++metrics_.submitted;
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

}  // namespace treeminer
