#include "CpuMiningWorker.h"

#include "hashapi/CpuHashBackend.h"
#include "hashapi/HashApiMatching.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace treeminer {
namespace {

constexpr std::uint32_t kMinCpuDifficulty = 8;

std::unique_ptr<hashapi::IHashBackend> makeCpuBackend()
{
    return std::make_unique<hashapi::CpuHashBackend>();
}

std::string digestFromProtocolPhc(const std::string& phc, std::uint32_t difficulty)
{
    const std::string parameters = "$argon2id$v=19$m=" + std::to_string(difficulty) + ",t=1,p=1$";
    if (phc.rfind(parameters, 0) != 0) {
        throw std::runtime_error("CPU backend returned a non-protocol Argon2id hash");
    }

    const std::size_t digest_separator = phc.rfind('$');
    if (digest_separator == std::string::npos || digest_separator + 1 >= phc.size()) {
        throw std::runtime_error("CPU backend returned an invalid PHC string");
    }
    return phc.substr(digest_separator + 1);
}

bool digestMatches(const CpuMiningWorker::Work& work,
                   const hashapi::HashApiMatch& match,
                   const std::string& digest)
{
    if (match.matched_pattern == work.target_pattern) {
        return digest.find(work.target_pattern) != std::string::npos;
    }
    if (match.matched_pattern == "XUNI") {
        return work.allow_xuni && hashapi::hasXuniMatch(digest);
    }
    return false;
}

} // namespace

CpuMiningWorker::CpuMiningWorker(Config config,
                                 DifficultyProvider difficulty_provider,
                                 WorkProvider work_provider,
                                 SubmitCallback submit_callback,
                                 ContinuePredicate should_continue,
                                 BackendFactory backend_factory)
    : config_(config),
      difficulty_provider_(std::move(difficulty_provider)),
      work_provider_(std::move(work_provider)),
      submit_callback_(std::move(submit_callback)),
      should_continue_(std::move(should_continue)),
      backend_factory_(std::move(backend_factory))
{
    if (config_.worker_count == 0) {
        throw std::invalid_argument("CPU worker count must be greater than zero");
    }
    if (config_.batch_size == 0 || config_.batch_size > hashapi::kMaxCpuBatchSize) {
        throw std::invalid_argument("CPU batch size must be between 1 and 10000");
    }
    if (!difficulty_provider_) {
        throw std::invalid_argument("CPU difficulty provider is required");
    }
    if (!work_provider_) {
        throw std::invalid_argument("CPU work provider is required");
    }
    if (!submit_callback_) {
        throw std::invalid_argument("CPU submit callback is required");
    }
    if (!should_continue_) {
        should_continue_ = [] { return true; };
    }
    if (!backend_factory_) {
        backend_factory_ = makeCpuBackend;
    }
}

CpuMiningWorker::~CpuMiningWorker()
{
    stop();
    join();
}

void CpuMiningWorker::start()
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load()) {
        return;
    }
    if (!threads_.empty()) {
        throw std::logic_error("CPU mining workers must be joined before restarting");
    }

    stop_requested_.store(false);
    attempts_.store(0);
    matches_.store(0);
    active_workers_.store(0);
    difficulty_.store(0);
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        worker_hashrates_.assign(config_.worker_count, 0.0);
        last_error_.clear();
        started_at_ = std::chrono::steady_clock::now();
        stopped_at_ = {};
    }
    running_.store(true);

    try {
        threads_.reserve(config_.worker_count);
        for (std::size_t index = 0; index < config_.worker_count; ++index) {
            threads_.emplace_back(&CpuMiningWorker::runWorker, this, index);
        }
    } catch (...) {
        stop_requested_.store(true);
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();
        running_.store(false);
        throw;
    }
}

void CpuMiningWorker::stop() noexcept
{
    stop_requested_.store(true);
}

void CpuMiningWorker::join() noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    running_.store(false);
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    if (started_at_ != std::chrono::steady_clock::time_point{} &&
        stopped_at_ == std::chrono::steady_clock::time_point{}) {
        stopped_at_ = std::chrono::steady_clock::now();
    }
}

bool CpuMiningWorker::isRunning() const noexcept
{
    return running_.load();
}

CpuMiningWorker::Stats CpuMiningWorker::stats() const
{
    Stats snapshot;
    snapshot.attempts = attempts_.load();
    snapshot.matches = matches_.load();
    snapshot.active_workers = active_workers_.load();
    snapshot.difficulty = difficulty_.load();
    snapshot.running = running_.load();

    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    snapshot.hashrate = 0.0;
    for (double worker_hashrate : worker_hashrates_) {
        snapshot.hashrate += worker_hashrate;
    }
    snapshot.last_error = last_error_;

    if (started_at_ != std::chrono::steady_clock::time_point{}) {
        const auto end = stopped_at_ == std::chrono::steady_clock::time_point{}
            ? std::chrono::steady_clock::now()
            : stopped_at_;
        const double seconds = std::chrono::duration<double>(end - started_at_).count();
        if (seconds > 0.0) {
            snapshot.average_hashrate = static_cast<double>(snapshot.attempts) / seconds;
        }
    }
    return snapshot;
}

double CpuMiningWorker::currentHashrate() const
{
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    double aggregate = 0.0;
    for (double worker_hashrate : worker_hashrates_) {
        aggregate += worker_hashrate;
    }
    return aggregate;
}

void CpuMiningWorker::recordError(const std::string& message) noexcept
{
    try {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        if (last_error_.empty()) {
            last_error_ = message;
        }
    } catch (...) {
        // Error reporting must not escape a worker thread.
    }
    stop_requested_.store(true);
}

void CpuMiningWorker::runWorker(std::size_t worker_index) noexcept
{
    active_workers_.fetch_add(1);
    try {
        auto backend = backend_factory_();
        if (!backend) {
            throw std::runtime_error("CPU backend factory returned null");
        }

        std::uint64_t attempts_since_match = 0;
        while (!stop_requested_.load() && should_continue_()) {
            const std::uint32_t difficulty = difficulty_provider_();
            if (difficulty < kMinCpuDifficulty) {
                throw std::runtime_error("CPU mining difficulty must be at least 8");
            }
            difficulty_.store(difficulty);

            const Work work = work_provider_();
            hashapi::HashApiRequest request;
            request.backend = "cpu";
            request.salt_hex = work.salt_hex;
            request.key_prefix = work.key_prefix;
            request.target_pattern = work.target_pattern;
            request.difficulty = difficulty;
            request.batch_size = config_.batch_size;
            request.device_id = static_cast<int>(worker_index);
            request.allow_xuni = work.allow_xuni;

            const hashapi::HashApiResult result = backend->runBatch(request);
            if (!result.ok) {
                throw std::runtime_error("CPU hash batch failed: " + result.error);
            }

            attempts_.fetch_add(result.attempts);
            {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                worker_hashrates_[worker_index] = result.hashrate;
            }

            std::size_t next_attempt_index = 0;
            for (const auto& match : result.matches) {
                if (match.attempt_index >= next_attempt_index) {
                    attempts_since_match += match.attempt_index - next_attempt_index + 1;
                    next_attempt_index = match.attempt_index + 1;
                }

                const std::string digest = digestFromProtocolPhc(match.hash, difficulty);
                if (!digestMatches(work, match, digest)) {
                    continue;
                }

                const double aggregate_hashrate = currentHashrate();
                {
                    std::lock_guard<std::mutex> submit_lock(submit_mutex_);
                    submit_callback_(work.salt_hex,
                                     match.key,
                                     digest,
                                     difficulty,
                                     static_cast<std::size_t>(attempts_since_match),
                                     static_cast<float>(aggregate_hashrate),
                                     "CPU");
                }
                matches_.fetch_add(1);
                attempts_since_match = 0;
            }

            if (result.attempts >= next_attempt_index) {
                attempts_since_match += result.attempts - next_attempt_index;
            }
        }
    } catch (const std::exception& ex) {
        recordError(ex.what());
    } catch (...) {
        recordError("unknown CPU mining worker error");
    }

    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        if (worker_index < worker_hashrates_.size()) {
            worker_hashrates_[worker_index] = 0.0;
        }
    }
    if (active_workers_.fetch_sub(1) == 1) {
        running_.store(false);
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stopped_at_ = std::chrono::steady_clock::now();
    }
}

} // namespace treeminer
