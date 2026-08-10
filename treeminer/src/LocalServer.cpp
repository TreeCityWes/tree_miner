#include "LocalServer.h"

#include <chrono>
#include <mutex>

#include <nlohmann/json.hpp>
#include "StatReporter.h"
#include "MiningCommon.h"
#include "MiningCoordinator.h"
#include "PlatformManager.h"
#include "submit/SubmissionManager.h"
#include "treeminer/IFindJournal.h"

extern bool globalPlatformMode;
extern std::unique_ptr<PlatformManager> globalPlatformManager;

static crow::SimpleApp s_app;
static treeminer::IFindJournal* s_journal = nullptr;
static treeminer::SubmissionManager* s_submission_manager = nullptr;

namespace {

std::mutex s_stats_cache_mutex;
std::string s_stats_cache;
std::chrono::steady_clock::time_point s_stats_cache_at{};

std::string buildStatsSnapshot() {
    nlohmann::json result = nlohmann::json::parse(getGpuStatsJson());

    if (s_submission_manager) {
        const auto metrics = s_submission_manager->metrics();
        const auto difficulty = s_submission_manager->lastObservedDifficulty();
        const std::uint64_t failed = metrics.transport_failures + metrics.parked_difficulty +
                                     metrics.parked_xuni + metrics.quarantined +
                                     metrics.permanently_invalid;

        result["difficultyStats"]["last_observed"] = difficulty ? nlohmann::json(*difficulty)
                                                                  : nlohmann::json(nullptr);
        result["difficultyStats"]["margin_in_effect"] = s_submission_manager->marginInEffect();
        result["difficultyStats"]["effective_mining_difficulty"] = difficulty
            ? nlohmann::json(static_cast<std::uint64_t>(*difficulty) +
                             s_submission_manager->marginInEffect())
            : nlohmann::json(nullptr);
        result["difficultyStats"]["margin_changes_total"] = metrics.margin_changes;
        result["pool"]["outage_duration_ms"] = s_submission_manager->outageDurationMs();
        result["submissions"] = {
            {"attempts_total", metrics.submitted},
            {"resubmissions_total", metrics.resubmitted},
            {"acked_total", metrics.acked},
            {"accepted_unconfirmed_total", metrics.accepted_unconfirmed},
            {"transport_failures_total", metrics.transport_failures},
            {"difficulty_rejections_total", metrics.parked_difficulty},
            {"xuni_window_rejections_total", metrics.parked_xuni},
            {"quarantined_total", metrics.quarantined},
            {"permanently_invalid_total", metrics.permanently_invalid},
            {"confirmation_retries_total", metrics.confirmation_retries},
            {"confirmed_via_lookup_total", metrics.reconciled_via_get_block},
            {"lying_200_total", metrics.lying_200_detected},
            {"difficulty_probes_total", metrics.probes},
            {"failed_attempts_total", failed},
            {"failure_rate_pct", metrics.submitted == 0
                                     ? 0.0
                                     : 100.0 * static_cast<double>(failed) /
                                           static_cast<double>(metrics.submitted)},
        };
    }

    if (s_journal) {
        const auto counts = s_journal->counts();
        result["journal"] = {
            {"pending", counts.pending},
            {"accepted_unconfirmed", counts.accepted_unconfirmed},
            {"parked_total", counts.parked},
            {"parked_difficulty", counts.parked_difficulty},
            {"parked_xuni", counts.parked_xuni},
            {"quarantined", counts.quarantined},
            {"acked_total", counts.acked_total},
            {"dead_total", counts.dead_total},
            {"permanently_invalid", counts.permanently_invalid},
        };
    }

    result["stats_cache_seconds"] = 2;
    return result.dump();
}

std::string getCachedStatsSnapshot() {
    std::lock_guard<std::mutex> lock(s_stats_cache_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (s_stats_cache.empty() || now - s_stats_cache_at >= std::chrono::seconds(2)) {
        s_stats_cache = buildStatsSnapshot();
        s_stats_cache_at = now;
    }
    return s_stats_cache;
}

} // namespace

crow::SimpleApp& getApp() {
    return s_app;
}

void startServer() {
    s_app.port(42069).multithreaded().run();
}

void setupRoutes(treeminer::IFindJournal* journal,
                 treeminer::SubmissionManager* submission_manager) {
    s_journal = journal;
    s_submission_manager = submission_manager;
    s_app.loglevel(crow::LogLevel::Warning);
    s_app.signal_clear();

    CROW_ROUTE(s_app, "/stats")
    ([](){
        crow::response response(getCachedStatsSnapshot());
        response.set_header("Content-Type", "application/json");
        response.set_header("Cache-Control", "no-store");
        return response;
    });

    CROW_ROUTE(s_app, "/api/v1/status")
    ([](){
        crow::response response(getCachedStatsSnapshot());
        response.set_header("Content-Type", "application/json");
        response.set_header("Cache-Control", "no-store");
        return response;
    });

    CROW_ROUTE(s_app, "/platform/status")
    ([](){
        nlohmann::json result;
        result["platform_mode"] = globalPlatformMode;
        auto ctx = MiningCoordinator::getInstance().getContext();
        result["mining_mode"] = ctx.mode == MiningMode::PLATFORM_MINING ? "platform" : "self";
        if (globalPlatformManager) {
            result["platform_state"] = platformStateToString(globalPlatformManager->getState());
            result["running"] = globalPlatformManager->isRunning();
            auto lease = globalPlatformManager->getLeaseManager().getLease();
            if (lease.has_value()) {
                result["lease_id"] = lease->lease_id;
                result["consumer_id"] = lease->consumer_id;
                result["consumer_address"] = lease->consumer_address;
                result["blocks_found"] = lease->blocks_found;
                result["remaining_sec"] = globalPlatformManager->getLeaseManager().remainingSeconds();
            }
        } else {
            result["platform_state"] = "disabled";
            result["running"] = false;
        }
        return result.dump();
    });
}
