#pragma once

#include <cstdint>
#include <functional>
#include <string>

std::string getDifficulty();
void updateDifficulty();
void updateDifficultyPeriodically();

// Fired with every successful difficulty sample (changed or not) so the submission
// manager's trend tracking and difficulty-unparking see the poller's observations too.
extern std::function<void(std::uint32_t)> globalDifficultyObserver;
