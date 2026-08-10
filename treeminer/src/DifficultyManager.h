#pragma once

#include <cstdint>
#include <functional>
#include <string>

std::string getDifficulty();
void updateDifficulty();
void updateDifficultyPeriodically();

// Last difficulty persisted to the local cache file ("difficulty.cache" in the working
// directory), or 0 if absent/invalid. Lets a restart during a server outage mine at the
// last known real difficulty instead of the hardcoded 42069 fallback.
int loadCachedDifficulty();

// Fired with every successful difficulty sample (changed or not) so the submission
// manager's trend tracking and difficulty-unparking see the poller's observations too.
extern std::function<void(std::uint32_t)> globalDifficultyObserver;
