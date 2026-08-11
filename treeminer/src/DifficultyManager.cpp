#include "DifficultyManager.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "HttpClient.h"
#include "MiningCommon.h"
#include "ConsoleLog.h"

std::function<void(std::uint32_t)> globalDifficultyObserver;

namespace {

constexpr const char* kDifficultyCacheFile = "difficulty.cache";
constexpr int kPoolDownFailureThreshold = 3;
int consecutiveDifficultyFailures = 0;

void persistDifficulty(int difficulty)
{
    // Best-effort: write-then-rename so a torn write can't leave a garbage value.
    const std::string tmp = std::string(kDifficultyCacheFile) + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
    {
        return;
    }
    out << difficulty << '\n';
    out.close();
    if (out.fail())
    {
        std::remove(tmp.c_str());
        return;
    }
    std::remove(kDifficultyCacheFile);
    std::rename(tmp.c_str(), kDifficultyCacheFile);
}

} // namespace

int loadCachedDifficulty()
{
    std::ifstream in(kDifficultyCacheFile);
    int value = 0;
    if (!(in >> value))
    {
        return 0;
    }
    if (value < 1 || value > 100000000)
    {
        return 0;
    }
    return value;
}

std::string getDifficulty()
{
    HttpClient httpClient;

    try
    {
        HttpResponse response = httpClient.HttpGet(globalRpcLink+"/difficulty", 5000);
        if (response.GetStatusCode() != 200)
        {
            throw std::runtime_error("Failed to get the difficulty: HTTP status code " + std::to_string(response.GetStatusCode()));
        }

        auto json_response = nlohmann::json::parse(response.GetBody());
        return json_response["difficulty"].get<std::string>();
    }
    catch (const nlohmann::json::parse_error &e)
    {
        throw std::runtime_error("JSON parsing error: " + std::string(e.what()));
    }
    catch (const std::exception &e)
    {
        throw std::runtime_error("Error: " + std::string(e.what()));
    }
}

void updateDifficulty()
{
    try
    {
        std::string difficultyStr = getDifficulty();
        int newDifficulty = std::stoi(difficultyStr);

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (globalDifficulty != newDifficulty)
            {
                globalDifficulty = newDifficulty;
            }
        }
        if (globalDifficultyObserver)
        {
            globalDifficultyObserver(static_cast<std::uint32_t>(newDifficulty));
        }
        persistDifficulty(newDifficulty);

        if (consecutiveDifficultyFailures > 0)
        {
            const int failures = consecutiveDifficultyFailures;
            consecutiveDifficultyFailures = 0;
            if (globalDifficultyEndpointDown.exchange(false))
            {
                ConsoleLog::event(ConsoleLog::Level::Ok, "POOL",
                                  "RECOVERED | endpoint=/difficulty | prior_failures=" +
                                      std::to_string(failures) +
                                      " | difficulty=" + std::to_string(newDifficulty));
            }
            else
            {
                ConsoleLog::event(ConsoleLog::Level::Info, "POOL",
                                  "difficulty poll restored | prior_failures=" +
                                      std::to_string(failures));
            }
        }
    }
    catch (const std::exception &e)
    {
        ++consecutiveDifficultyFailures;
        if (consecutiveDifficultyFailures == 1)
        {
            ConsoleLog::event(ConsoleLog::Level::Warn, "POOL",
                              "difficulty poll failed | failure=1/" +
                                  std::to_string(kPoolDownFailureThreshold) +
                                  " | " + e.what());
        }
        if (consecutiveDifficultyFailures == kPoolDownFailureThreshold)
        {
            globalDifficultyEndpointDown.store(true);
            ConsoleLog::event(ConsoleLog::Level::Error, "POOL",
                              "DOWN | endpoint=/difficulty | consecutive_failures=" +
                                  std::to_string(consecutiveDifficultyFailures) +
                                  " | polling continues every 10s | " + e.what());
        }
    }
}

void updateDifficultyPeriodically()
{
    while (running)
    {
        updateDifficulty();
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}
