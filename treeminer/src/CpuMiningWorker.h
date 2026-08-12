#pragma once

#include "MiningCommon.h"
#include "hashapi/HashApiTypes.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace treeminer {

// Owns a group of independent CPU hashing threads. Mutable miner state is supplied
// through callbacks so this class does not own or retain references to global state.
class CpuMiningWorker {
public:
    struct Config {
        std::size_t worker_count = 1;
        std::size_t batch_size = 1;
        // CPU hashing is only worthwhile near the difficulty floor: m IS the Argon2
        // memory cost, so at high difficulty a CPU burns power for a negligible share
        // of the network. Workers idle (cheap poll, no hashing) while the observed
        // difficulty exceeds this ceiling and resume automatically when it falls.
        // 0 disables the gate (hash at any difficulty).
        std::uint32_t max_difficulty = 0;
    };

    struct Work {
        std::string salt_hex;
        std::string key_prefix;
        std::string target_pattern = "XEN11";
        bool allow_xuni = true;
    };

    struct Stats {
        std::uint64_t attempts = 0;
        std::uint64_t matches = 0;
        double hashrate = 0.0;
        double average_hashrate = 0.0;
        std::size_t active_workers = 0;
        std::uint32_t difficulty = 0;
        bool running = false;
        // True while workers are alive but idling because difficulty > max_difficulty.
        bool paused_for_difficulty = false;
        std::uint32_t max_difficulty = 0;   // the configured ceiling (0 = no gate)
        std::string last_error;
    };

    using DifficultyProvider = std::function<std::uint32_t()>;
    using WorkProvider = std::function<Work()>;
    using ContinuePredicate = std::function<bool()>;
    using BackendFactory = std::function<std::unique_ptr<hashapi::IHashBackend>()>;

    CpuMiningWorker(Config config,
                    DifficultyProvider difficulty_provider,
                    WorkProvider work_provider,
                    SubmitCallback submit_callback,
                    ContinuePredicate should_continue = {},
                    BackendFactory backend_factory = {});
    ~CpuMiningWorker();

    CpuMiningWorker(const CpuMiningWorker&) = delete;
    CpuMiningWorker& operator=(const CpuMiningWorker&) = delete;
    CpuMiningWorker(CpuMiningWorker&&) = delete;
    CpuMiningWorker& operator=(CpuMiningWorker&&) = delete;

    void start();
    void stop() noexcept;
    void join() noexcept;

    bool isRunning() const noexcept;
    Stats stats() const;

private:
    void runWorker(std::size_t worker_index) noexcept;
    void recordError(const std::string& message) noexcept;
    double currentHashrate() const;

    Config config_;
    DifficultyProvider difficulty_provider_;
    WorkProvider work_provider_;
    SubmitCallback submit_callback_;
    ContinuePredicate should_continue_;
    BackendFactory backend_factory_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> attempts_{0};
    std::atomic<std::uint64_t> matches_{0};
    std::atomic<std::size_t> active_workers_{0};
    std::atomic<std::uint32_t> difficulty_{0};
    std::atomic<bool> paused_for_difficulty_{false};

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex stats_mutex_;
    mutable std::mutex submit_mutex_;
    std::vector<std::thread> threads_;
    std::vector<double> worker_hashrates_;
    std::string last_error_;
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point stopped_at_{};
};

} // namespace treeminer
