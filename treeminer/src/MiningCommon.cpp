#include "MiningCommon.h"

#include <cstdint>
#include <ctime>
#include <memory>
#include <utility>

std::atomic<bool> running = true;
std::atomic<int> globalDifficulty = 1727;
std::atomic<bool> globalDifficultyEndpointDown = false;
std::atomic<int> globalDifficultyMargin = 0;
std::atomic<int> globalMiningDifficultyLock = 0;
std::atomic<int> globalLockLiveLanes = 0;
std::mutex mtx;

namespace {
int liveMiningDifficulty() {
    const int difficulty = globalDifficulty.load();
    const int margin = globalDifficultyMargin.load();
    // Defensive: a negative or overflowing sum would mean mining at a memory cost the server
    // would reject outright, which is worse than ignoring the margin entirely.
    if (margin <= 0) {
        return difficulty;
    }
    const long long sum = static_cast<long long>(difficulty) + static_cast<long long>(margin);
    if (sum > static_cast<long long>(INT32_MAX)) {
        return difficulty;
    }
    return static_cast<int>(sum);
}
} // namespace

int effectiveMiningDifficulty() {
    const int lock = globalMiningDifficultyLock.load();
    if (lock > 0) {
        return lock;
    }
    return liveMiningDifficulty();
}

int laneMiningDifficulty(int streamIndex) {
    const int lock = globalMiningDifficultyLock.load();
    if (lock > 0 && streamIndex >= globalLockLiveLanes.load()) {
        return lock;
    }
    return liveMiningDifficulty();
}
std::mutex coutmtx;

std::atomic<bool> globalFatalDurabilityFailure{false};

namespace {
// Mutex-guarded string rather than an atomic pointer to a fixed literal: the reason must
// carry the live SQLite/errno text from the failure site, and this lock is touched only
// on declaration and on stats reads — never on the mining hot path.
std::mutex fatalDurabilityReasonMutex;
std::string fatalDurabilityReason;
} // namespace

void declareFatalDurabilityFailure(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(fatalDurabilityReasonMutex);
        // First failure wins. A second double-failure (a later find on the same broken
        // disk) would overwrite the original diagnosis with an echo of it.
        if (fatalDurabilityReason.empty()) {
            fatalDurabilityReason =
                reason.empty() ? "unspecified durability failure" : reason;
        }
    }
    // Publish the reason BEFORE raising the flag so any reader that observes the flag is
    // guaranteed to find a complete reason string behind it.
    globalFatalDurabilityFailure.store(true);
    // Stop mining now: the GPU device loops (runMiningOnDevice -> MineUnit::runMineLoop)
    // and the CPU workers' should_continue predicate all poll `running`, and main()'s
    // wait loop exits on it. A miner that cannot persist finds is destroying every
    // future find it makes; main() turns the flag into a nonzero exit for the supervisor.
    running.store(false);
}

std::string fatalDurabilityFailureReason()
{
    std::lock_guard<std::mutex> lock(fatalDurabilityReasonMutex);
    return fatalDurabilityReason;
}

namespace {
std::shared_ptr<const MiningIdentityConfig> miningIdentity =
	std::make_shared<const MiningIdentityConfig>(
		MiningIdentityConfig{"0x123456789", "", ""});
std::mutex miningIdentityWriteMutex;

template <typename Update>
void updateMiningIdentity(Update&& update)
{
	std::lock_guard<std::mutex> lock(miningIdentityWriteMutex);
	auto next = std::make_shared<MiningIdentityConfig>(
		*std::atomic_load_explicit(&miningIdentity, std::memory_order_acquire));
	update(*next);
	std::atomic_store_explicit(
		&miningIdentity,
		std::shared_ptr<const MiningIdentityConfig>(std::move(next)),
		std::memory_order_release);
}
} // namespace

std::shared_ptr<const MiningIdentityConfig> miningIdentitySnapshot()
{
	return std::atomic_load_explicit(&miningIdentity, std::memory_order_acquire);
}

void setMiningUserAddress(std::string address)
{
	updateMiningIdentity([&](MiningIdentityConfig& config) {
		config.userAddress = std::move(address);
	});
}

void setSelfMiningPrefix(std::string prefix)
{
	updateMiningIdentity([&](MiningIdentityConfig& config) {
		config.selfMiningPrefix = std::move(prefix);
	});
}

void setTestBlockPattern(std::string pattern)
{
	updateMiningIdentity([&](MiningIdentityConfig& config) {
		config.testBlockPattern = std::move(pattern);
	});
}

std::string globalDevfeeAddress = "0x24691E54aFafe2416a8252097C9Ca67557271475";
std::string globalEcoDevfeeAddress = "";
std::atomic<int> globalDevfeePermillage = 1; // per 1000
std::string machineId = "00000";

std::function<bool(TreeminerStats&)> globalTreeminerStatsProvider;
std::function<bool(SubmissionLineStats&)> globalSubmissionLineStatsProvider;

std::map<int, std::pair<gpuInfo, std::chrono::steady_clock::time_point>> globalGpuInfos;
std::mutex globalGpuInfosMutex;

std::atomic<int> globalNormalBlockCount = 0;
std::atomic<int> globalSuperBlockCount = 0;
std::atomic<int> globalXuniBlockCount = 0;
std::atomic<int> globalFailedBlockCount = 0;

std::atomic<long> globalHashCount = 0;
std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();

std::string globalRpcLink = "http://xenblocks.io";
std::size_t globalMaxBatchSize = 0; // 0 = auto (use all free GPU memory)
std::size_t globalCudaStreamsPerDevice = 1;
std::atomic<std::size_t> globalQueuedXnm{0};
std::atomic<std::size_t> globalQueuedXuni{0};
std::atomic<LastSubmissionState> globalLastSubmission{LastSubmissionState::None};
std::atomic<treeminer::CircuitBreaker::State> globalNetworkState{
	treeminer::CircuitBreaker::State::Closed};
std::atomic<std::size_t> globalCpuWorkers{0};
std::atomic<double> globalCpuHashrate{0.0};
std::string globalDashboardBind = "0.0.0.0";
int globalDashboardPort = 42069;

const char* submissionStateLabel(LastSubmissionState state)
{
	switch (state) {
		case LastSubmissionState::Accepted: return "accepted";
		case LastSubmissionState::Unconfirmed: return "confirming";
		case LastSubmissionState::Retry: return "retrying";
		case LastSubmissionState::Parked: return "held";
		case LastSubmissionState::Failed: return "rejected";
		default: return "none";
	}
}

const char* networkStateLabel(treeminer::CircuitBreaker::State state)
{
	switch (state) {
		case treeminer::CircuitBreaker::State::Open: return "offline";
		case treeminer::CircuitBreaker::State::HalfOpen: return "probing";
		default: return "online";
	}
}

bool isWithinXuniWindow()
{
	const std::time_t now = std::time(nullptr);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	return local.tm_min < 5 || local.tm_min >= 55;
}
