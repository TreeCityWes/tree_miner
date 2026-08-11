#include "LocalServer.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "DashboardPage.h"
#include "StatReporter.h"
#include "MiningCommon.h"
#include "MiningCoordinator.h"
#include "PlatformManager.h"

extern bool globalPlatformMode;
extern std::unique_ptr<PlatformManager> globalPlatformManager;

static crow::SimpleApp s_app;

crow::SimpleApp& getApp() {
    return s_app;
}

std::string getConsoleUrl() {
    static const std::string url = [] {
        std::string address = "127.0.0.1";
#ifndef _WIN32
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(53);
            inet_pton(AF_INET, "1.1.1.1", &destination.sin_addr);
            if (connect(fd, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0) {
                sockaddr_in local{};
                socklen_t length = sizeof(local);
                if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
                    char buffer[INET_ADDRSTRLEN]{};
                    if (inet_ntop(AF_INET, &local.sin_addr, buffer, sizeof(buffer))) {
                        address = buffer;
                    }
                }
            }
            close(fd);
        }
#endif
        return "http://" + address + ":42069";
    }();
    return url;
}

void startServer() {
    s_app.bindaddr("0.0.0.0").port(42069).multithreaded().run();
}

void setupRoutes() {
    s_app.loglevel(crow::LogLevel::Warning);
    s_app.signal_clear();

    CROW_ROUTE(s_app, "/stats")
    ([](){
        auto stats = getGpuStatsJson();
        return stats;
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
