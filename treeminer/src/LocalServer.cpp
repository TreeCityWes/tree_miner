#include "LocalServer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "DashboardPage.h"
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
    // Base snapshot from StatReporter. This already carries the fatal durability state
    // ("fatalDurabilityFailure" + reason, review finding 6), so /stats and
    // /api/v1/status expose it without a second read of the flag here.
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
    result["console"] = {
        {"bind", globalDashboardBind},
        {"open", getConsoleUrl(globalDashboardBind)},
        {"urls", dashboardAdvertisedAddresses(globalDashboardBind)}
    };
    return result.dump();
}

std::string getCachedStatsSnapshot() {
    std::lock_guard<std::mutex> lock(s_stats_cache_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (s_stats_cache.empty() || now - s_stats_cache_at >= std::chrono::seconds(2)) {
        try {
            s_stats_cache = buildStatsSnapshot();
        } catch (const std::exception& e) {
            s_stats_cache = nlohmann::json{{"ok", false}, {"error", e.what()}}.dump();
        }
        s_stats_cache_at = now;
    }
    return s_stats_cache;
}

} // namespace

crow::SimpleApp& getApp() {
    return s_app;
}

bool isValidDashboardBind(const std::string& address) {
    in_addr ipv4{};
    in6_addr ipv6{};
    return inet_pton(AF_INET, address.c_str(), &ipv4) == 1 ||
           inet_pton(AF_INET6, address.c_str(), &ipv6) == 1;
}

bool isLoopbackDashboardBind(const std::string& address) {
    return address == "127.0.0.1" || address == "::1";
}

namespace {

bool isUnusableAdvertisedHost(const std::string& host) {
    if (host.empty() || host == "0.0.0.0" || host == "::" ||
        host == "127.0.0.1" || host == "::1") {
        return true;
    }
    if (host.rfind("169.254.", 0) == 0) return true;          // IPv4 link-local
    if (host.rfind("fe80:", 0) == 0 || host.rfind("FE80:", 0) == 0) return true;
    return false;
}

void appendUniqueHost(std::vector<std::string>& hosts, const std::string& host) {
    if (isUnusableAdvertisedHost(host)) return;
    if (std::find(hosts.begin(), hosts.end(), host) != hosts.end()) return;
    hosts.push_back(host);
}

void collectInterfaceAddresses(std::vector<std::string>& ipv4, std::vector<std::string>& ipv6) {
#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG error = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size);
    if (error == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR) {
            return;
        }
    } else if (error != NO_ERROR) {
        return;
    }
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr) continue;
            const int family = unicast->Address.lpSockaddr->sa_family;
            char text[INET6_ADDRSTRLEN]{};
            if (family == AF_INET) {
                const auto* addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text))) {
                    appendUniqueHost(ipv4, text);
                }
            } else if (family == AF_INET6) {
                const auto* addr = reinterpret_cast<sockaddr_in6*>(unicast->Address.lpSockaddr);
                if (inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text))) {
                    appendUniqueHost(ipv6, text);
                }
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || interfaces == nullptr) return;
    for (ifaddrs* iface = interfaces; iface != nullptr; iface = iface->ifa_next) {
        if (iface->ifa_addr == nullptr) continue;
        if ((iface->ifa_flags & IFF_UP) == 0) continue;
        if ((iface->ifa_flags & IFF_LOOPBACK) != 0) continue;
        char text[INET6_ADDRSTRLEN]{};
        if (iface->ifa_addr->sa_family == AF_INET) {
            const auto* addr = reinterpret_cast<sockaddr_in*>(iface->ifa_addr);
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text))) {
                appendUniqueHost(ipv4, text);
            }
        } else if (iface->ifa_addr->sa_family == AF_INET6) {
            const auto* addr = reinterpret_cast<sockaddr_in6*>(iface->ifa_addr);
            if (inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text))) {
                appendUniqueHost(ipv6, text);
            }
        }
    }
    freeifaddrs(interfaces);
#endif
}

} // namespace

std::string formatDashboardUrl(const std::string& host) {
    const bool ipv6 = host.find(':') != std::string::npos;
    return "http://" + (ipv6 ? "[" + host + "]" : host) + ":" +
           std::to_string(globalDashboardPort);
}

std::vector<std::string> dashboardAdvertisedAddresses(const std::string& bind_address) {
    if (bind_address != "0.0.0.0" && bind_address != "::") {
        if (!isUnusableAdvertisedHost(bind_address)) return {bind_address};
        if (isLoopbackDashboardBind(bind_address)) return {bind_address};
        return {};
    }

    std::vector<std::string> ipv4;
    std::vector<std::string> ipv6;
    collectInterfaceAddresses(ipv4, ipv6);

    std::vector<std::string> hosts = ipv4;
    hosts.insert(hosts.end(), ipv6.begin(), ipv6.end());
    if (hosts.empty()) {
        hosts.push_back(bind_address == "::" ? "::1" : "127.0.0.1");
    }
    return hosts;
}

std::string getConsoleUrl(const std::string& bind_address) {
    const auto hosts = dashboardAdvertisedAddresses(bind_address);
    return formatDashboardUrl(hosts.empty() ? "127.0.0.1" : hosts.front());
}

std::string dashboardReadyMessage(const std::string& bind_address) {
    const auto hosts = dashboardAdvertisedAddresses(bind_address);
    std::ostringstream message;
    if (isLoopbackDashboardBind(bind_address)) {
        message << "Dashboard ready — open " << formatDashboardUrl(bind_address)
                << " (this machine only)\n";
        return message.str();
    }
    if (bind_address == "0.0.0.0" || bind_address == "::") {
        message << "Dashboard listening on all interfaces, port "
                << globalDashboardPort << "\n";
    } else {
        message << "Dashboard listening on " << bind_address << ":"
                << globalDashboardPort << "\n";
    }
    for (const auto& host : hosts) {
        message << "  open  " << formatDashboardUrl(host) << "\n";
    }
    return message.str();
}

void startServer(const std::string& bind_address) {
    s_app.bindaddr(bind_address).port(globalDashboardPort).multithreaded().run();
}

void setupRoutes(treeminer::IFindJournal* journal,
                 treeminer::SubmissionManager* submission_manager) {
    s_journal = journal;
    s_submission_manager = submission_manager;
    s_app.loglevel(crow::LogLevel::Warning);
    s_app.signal_clear();

    CROW_ROUTE(s_app, "/healthz")
    ([](){
        crow::response response(R"({"ok":true})");
        response.set_header("Content-Type", "application/json");
        response.set_header("Cache-Control", "no-store");
        return response;
    });

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

    CROW_ROUTE(s_app, "/")
    ([](){
        crow::response response{std::string(treeminer::dashboard::kPage)};
        response.set_header("Content-Type", "text/html; charset=utf-8");
        response.set_header("Cache-Control", "no-store");
        return response;
    });

    CROW_ROUTE(s_app, "/assets/hashfield.webp")
    ([](){
        crow::response response;
        const std::filesystem::path asset = "res/dashboard/hashfield.webp";
        if (!std::filesystem::exists(asset)) {
            response.code = 404;
            response.body = "asset unavailable";
            return response;
        }
        response.set_static_file_info(asset.string());
        response.set_header("Cache-Control", "public, max-age=86400");
        return response;
    });

    CROW_ROUTE(s_app, "/api/rig")
    ([](){
        crow::response response{getMinerDashboardData().dump()};
        response.set_header("Content-Type", "application/json");
        response.set_header("Cache-Control", "no-store");
        return response;
    });
}
