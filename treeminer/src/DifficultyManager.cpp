#include "DifficultyManager.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "HttpClient.h"
#include "Logger.h"
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
                ConsoleLog::event(ConsoleLog::Level::Ok, "NETWORK",
                                  "difficulty restored | current=" +
                                      std::to_string(newDifficulty));
            }
            else
            {
                ConsoleLog::event(ConsoleLog::Level::Info, "NETWORK",
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
            // Keep the cause (DNS/timeout/HTTP) on the first failure — it's the operator's
            // first clue whether the pool, the link, or DNS is the problem.
            ConsoleLog::event(ConsoleLog::Level::Warn, "NETWORK",
                              std::string("difficulty poll failed; using cached value and retrying — ") +
                                  e.what());
        }
        if (consecutiveDifficultyFailures == kPoolDownFailureThreshold)
        {
            globalDifficultyEndpointDown.store(true);
            // Escalate once when we cross the threshold: this is the durable "the difficulty
            // endpoint is down" signal, distinct from a single transient poll miss.
            ConsoleLog::event(ConsoleLog::Level::Error, "NETWORK",
                              "difficulty endpoint DOWN after " +
                                  std::to_string(consecutiveDifficultyFailures) +
                                  " failures — mining continues on cached difficulty; polling every 10s");
        }
    }
}

void updateDifficultyPeriodically()
{
    while (running)
    {
        updateDifficulty();
        if (!interruptibleShutdownSleep(std::chrono::seconds(10))) {
            break;
        }
    }
}
