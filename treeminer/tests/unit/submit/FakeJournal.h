#pragma once
// In-memory IFindJournal fake for unit tests. Mirrors the documented contract closely
// enough for submitter testing; the real SQLite implementation lives in src/journal/.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "treeminer/IFindJournal.h"
#include "treeminer/Types.h"

namespace treeminer_test {

class FakeJournal : public treeminer::IFindJournal {
public:
    using FindRecord = treeminer::FindRecord;
    using FoundPayload = treeminer::FoundPayload;
    using Classification = treeminer::Classification;
    using FindStatus = treeminer::FindStatus;
    using FindKind = treeminer::FindKind;

    std::int64_t append(const FoundPayload& payload) override {
        for (const auto& r : records_) {
            if (r.payload.key == payload.key) {
                return r.id;  // idempotent local capture
            }
        }
        FindRecord r;
        r.id = next_id_++;
        r.payload = payload;
        r.status = FindStatus::Pending;
        records_.push_back(r);
        return r.id;
    }

    std::vector<FindRecord> fetchEligible(const std::string& now_utc,
                                          std::size_t limit) override {
        std::vector<FindRecord> out;
        for (const auto& r : records_) {  // records_ is id-ordered == oldest-first
            if (r.status != FindStatus::Pending) {
                continue;
            }
            if (r.next_attempt_at && *r.next_attempt_at > now_utc) {
                continue;  // ISO-8601 sorts lexicographically
            }
            out.push_back(r);
            if (out.size() >= limit) {
                break;
            }
        }
        return out;
    }

    // Same eligibility rules as fetchEligible, restricted to one kind. The LIMIT applies
    // after the kind filter, exactly as the SQL does — that is the whole point of the
    // method, so the fake must not shortcut it.
    std::vector<FindRecord> fetchEligibleOfKind(FindKind kind, const std::string& now_utc,
                                                std::size_t limit) override {
        std::vector<FindRecord> out;
        for (const auto& r : records_) {  // records_ is id-ordered == oldest-first
            if (r.status != FindStatus::Pending || r.payload.kind != kind) {
                continue;
            }
            if (r.next_attempt_at && *r.next_attempt_at > now_utc) {
                continue;  // ISO-8601 sorts lexicographically
            }
            out.push_back(r);
            if (out.size() >= limit) {
                break;
            }
        }
        return out;
    }

    std::vector<FindRecord> fetchAwaitingConfirmation(const std::string& now_utc,
                                                      std::size_t limit) override {
        std::vector<FindRecord> out;
        for (const auto& r : records_) {  // oldest-first
            if (r.status != FindStatus::AcceptedUnconfirmed) {
                continue;
            }
            if (r.next_attempt_at && *r.next_attempt_at > now_utc) {
                continue;
            }
            out.push_back(r);
            if (out.size() >= limit) {
                break;
            }
        }
        return out;
    }

    std::optional<FindRecord> getById(std::int64_t id) override {
        FindRecord* r = find_(id);
        if (!r) {
            return std::nullopt;
        }
        return *r;
    }

    void recordAttempt(std::int64_t id, const Classification& c,
                       std::optional<int> http_status, const std::string& response_body,
                       const std::optional<std::string>& next_attempt_at,
                       const std::string& now_utc) override {
        FindRecord* r = find_(id);
        if (!r) {
            return;
        }
        r->status = c.next_status;
        r->status_reason = c.reason;
        r->attempt_count += 1;
        r->next_attempt_at = next_attempt_at;
        r->last_attempt_at = now_utc;
        r->last_http_status = http_status;
        r->last_response = response_body;
        if (c.next_status == FindStatus::Acked) {
            r->confirmed_at = now_utc;
        }
        attempts_recorded.push_back({id, c, http_status, response_body, next_attempt_at});
    }

    std::size_t unparkForDifficulty(std::uint32_t current_difficulty) override {
        std::size_t n = 0;
        for (auto& r : records_) {
            if (r.status == FindStatus::ParkedDifficulty &&
                r.payload.memory_cost >= current_difficulty) {
                r.status = FindStatus::Pending;
                r.next_attempt_at.reset();
                ++n;
            }
        }
        unpark_difficulty_calls.push_back(current_difficulty);
        return n;
    }

    std::size_t unparkXuniForWindow(std::int32_t max_windows) override {
        std::size_t n = 0;
        for (auto& r : records_) {
            if (r.status != FindStatus::ParkedXuniWindow) {
                continue;
            }
            if (r.xuni_windows_tried >= max_windows) {
                r.status = FindStatus::Dead;
                continue;
            }
            r.xuni_windows_tried += 1;
            r.status = FindStatus::Pending;
            r.next_attempt_at.reset();
            ++n;
        }
        ++unpark_xuni_calls;
        return n;
    }

    RecoveryStats recoverOnStartup() override {
        RecoveryStats s;
        for (auto& r : records_) {
            if (r.status == FindStatus::Submitting) {
                r.status = FindStatus::Pending;
            }
            switch (r.status) {
                case FindStatus::Pending: ++s.pending; break;
                case FindStatus::AcceptedUnconfirmed: ++s.accepted_unconfirmed; break;
                case FindStatus::ParkedDifficulty: ++s.parked_difficulty; break;
                case FindStatus::ParkedXuniWindow: ++s.parked_xuni; break;
                case FindStatus::Quarantined: ++s.quarantined; break;
                case FindStatus::Acked: ++s.acked; break;
                case FindStatus::Dead: ++s.dead; break;
                case FindStatus::PermanentlyInvalid: ++s.invalid; break;
                default: break;
            }
        }
        return s;
    }

    void recordDifficulty(std::uint32_t difficulty, const std::string& at_utc) override {
        difficulty_log.push_back({difficulty, at_utc});
    }

    std::optional<std::uint32_t> lastKnownDifficulty() override {
        if (difficulty_log.empty()) {
            return std::nullopt;
        }
        return difficulty_log.back().first;
    }

    Counts counts() override {
        Counts c;
        for (const auto& r : records_) {
            if (r.status == FindStatus::Pending ||
                r.status == FindStatus::AcceptedUnconfirmed) {
                if (r.payload.kind == FindKind::XEN11) {
                    ++c.queued_xen11;
                } else {
                    ++c.queued_xuni;
                }
            }
            switch (r.status) {
                case FindStatus::Pending: ++c.pending; break;
                case FindStatus::ParkedDifficulty:
                    ++c.parked;
                    ++c.parked_difficulty;
                    break;
                case FindStatus::ParkedXuniWindow:
                    ++c.parked;
                    ++c.parked_xuni;
                    break;
                case FindStatus::Quarantined: ++c.quarantined; break;
                case FindStatus::Acked: ++c.acked_total; break;
                case FindStatus::Dead: ++c.dead_total; break;
                case FindStatus::AcceptedUnconfirmed: ++c.accepted_unconfirmed; break;
                case FindStatus::PermanentlyInvalid: ++c.permanently_invalid; break;
                default: break;
            }
        }
        return c;
    }

    // --- test access ---
    FindRecord* find_(std::int64_t id) {
        for (auto& r : records_) {
            if (r.id == id) {
                return &r;
            }
        }
        return nullptr;
    }
    const FindRecord& record(std::int64_t id) { return *find_(id); }

    struct AttemptLog {
        std::int64_t id;
        Classification c;
        std::optional<int> http_status;
        std::string body;
        std::optional<std::string> next_attempt_at;
    };

    std::vector<FindRecord> records_;
    std::int64_t next_id_ = 1;
    std::vector<AttemptLog> attempts_recorded;
    std::vector<std::uint32_t> unpark_difficulty_calls;
    int unpark_xuni_calls = 0;
    std::vector<std::pair<std::uint32_t, std::string>> difficulty_log;
};

}  // namespace treeminer_test
