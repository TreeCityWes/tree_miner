#include "CpuMiningWorker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

class FakeCpuBackend final : public hashapi::IHashBackend {
public:
    hashapi::HashApiResult runBatch(const hashapi::HashApiRequest& request) override
    {
        hashapi::HashApiResult result;
        result.ok = true;
        result.backend = "cpu";
        result.attempts = request.batch_size;
        result.batch_size = request.batch_size;
        result.hashrate = 2500.0;
        result.matches.push_back({
            std::string(64, 'a'),
            "$argon2id$v=19$m=" + std::to_string(request.difficulty) +
                ",t=1,p=1$c2FsdHNhbHQ$prefixXEN11suffix",
            "XEN11",
            0,
            false,
        });
        return result;
    }
};

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        std::mutex mutex;
        std::condition_variable submitted;
        std::atomic<bool> continue_mining{true};
        bool callback_called = false;

        treeminer::CpuMiningWorker worker(
            {1, 8},
            [] { return std::uint32_t{1100}; },
            [] {
                return treeminer::CpuMiningWorker::Work{
                    "e4bb184781bbc9c7004e8dafd4a9b49d203bc9bc",
                    "",
                    "XEN11",
                    false,
                };
            },
            [&](const std::string& salt,
                const std::string& key,
                const std::string& digest,
                std::uint32_t difficulty,
                std::size_t attempts,
                float hashrate,
                const std::string& source) {
                require(salt == "e4bb184781bbc9c7004e8dafd4a9b49d203bc9bc", "wrong salt");
                require(key == std::string(64, 'a'), "wrong key");
                require(digest == "prefixXEN11suffix", "PHC digest was not extracted");
                require(difficulty == 1100, "wrong difficulty");
                require(attempts == 1, "wrong attempts-to-match count");
                require(hashrate == 2500.0f, "wrong aggregate hashrate");
                require(source == "CPU", "wrong source label");
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    callback_called = true;
                }
                continue_mining.store(false);
                submitted.notify_one();
            },
            [&] { return continue_mining.load(); },
            [] { return std::make_unique<FakeCpuBackend>(); });

        worker.start();
        {
            std::unique_lock<std::mutex> lock(mutex);
            require(submitted.wait_for(lock, std::chrono::seconds(2), [&] { return callback_called; }),
                    "CPU worker did not submit a match");
        }
        worker.stop();
        worker.join();

        const auto stats = worker.stats();
        require(!worker.isRunning(), "CPU worker remained running after join");
        require(stats.attempts == 8, "wrong aggregate attempt count");
        require(stats.matches == 1, "wrong aggregate match count");
        require(stats.active_workers == 0, "worker remained active after join");
        require(stats.last_error.empty(), "worker reported an unexpected error");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
