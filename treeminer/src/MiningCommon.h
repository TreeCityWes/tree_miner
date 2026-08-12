#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <functional>
#include <string>
#include <map>
#include <memory>
#include <chrono>

#include "submit/CircuitBreaker.h"

constexpr std::size_t HASH_LENGTH = 64;
constexpr std::size_t MAX_SUBMIT_RETRIES = 5;

const std::string CONFIG_FILENAME = "config.txt";
const std::string DEVFEE_PREFIX = "FFFFFFFF";
const std::string ECODEVFEE_PREFIX = "EEEEEEEE";

// --- Hashpower Marketplace types ---

constexpr std::size_t PLATFORM_PREFIX_LENGTH = 16;

enum class MiningMode {
	SELF_MINING,
	PLATFORM_MINING
};

struct MiningContext {
	MiningMode mode = MiningMode::SELF_MINING;
	std::string address;       // target mining address (user's own or consumer's)
	std::string prefix;        // hex prefix for key generation (16 chars for platform)
	std::string consumer_id;   // platform consumer identifier
	std::string lease_id;      // platform lease identifier
};

// Remotely-updatable mining identity. Readers take one immutable snapshot so a
// batch cannot observe a mixture of values while MQTT applies a configuration
// update. atomic_load on the shared snapshot keeps the mining hot path free of
// the writer mutex.
struct MiningIdentityConfig {
	std::string userAddress;
	std::string selfMiningPrefix;
	std::string testBlockPattern;
};

std::shared_ptr<const MiningIdentityConfig> miningIdentitySnapshot();
void setMiningUserAddress(std::string address);
void setSelfMiningPrefix(std::string prefix);
void setTestBlockPattern(std::string pattern);
extern std::string globalDevfeeAddress;
extern std::string globalEcoDevfeeAddress;
extern std::atomic<int> globalDevfeePermillage; // per 1000
extern std::string machineId;

extern std::atomic<int> globalDifficulty;
// True after repeated /difficulty failures; lets the terminal report an outage even when
// no find is currently eligible for the submission circuit breaker.
extern std::atomic<bool> globalDifficultyEndpointDown;

// Headroom in KiB baked into newly mined hashes on top of globalDifficulty (PLAN §5, §10.7).
// Published by the SubmissionManager's margin policy; 0 unless an operator enables it.
extern std::atomic<int> globalDifficultyMargin;

// The memory cost new batches must actually mine at. Every producer of a hash — batch sizing,
// the kernel request, and the m= baked into the PHC string — must agree on this one value,
// or the miner would advertise a cost it did not pay.
int effectiveMiningDifficulty();

extern std::mutex mtx;
extern std::atomic<bool> running;
extern std::mutex coutmtx;

// --- Fatal durability state (security review finding 6) ---
// Raised when a find could be persisted by NEITHER the SQLite journal NOR the fallback
// sink. From that moment every future find would be destroyed on arrival, so continuing
// to mine is strictly worse than dying: declareFatalDurabilityFailure() records the
// reason, stops the mining loops via `running`, and main() translates the flag into a
// NONZERO process exit so a supervisor (systemd Restart=always) restarts the miner
// against a hopefully-recovered disk. std::exit is deliberately never called from the
// mining callback thread.
extern std::atomic<bool> globalFatalDurabilityFailure;
// First declaration wins: the first failure is the one that diagnosed the disk; later
// ones are echoes. Thread-safe; callable from any mining/callback thread.
void declareFatalDurabilityFailure(const std::string& reason);
// Empty until the flag is raised. Safe from the stats threads (mutex-guarded copy).
std::string fatalDurabilityFailureReason();

extern std::atomic<int> globalNormalBlockCount;
extern std::atomic<int> globalSuperBlockCount;
extern std::atomic<int> globalXuniBlockCount;
extern std::atomic<int> globalFailedBlockCount;

extern std::chrono::system_clock::time_point start_time;
extern std::atomic<long> globalHashCount;

extern std::string globalRpcLink;
extern std::size_t globalMaxBatchSize;
extern std::size_t globalCudaStreamsPerDevice;

enum class LastSubmissionState {
	None,
	Accepted,
	Unconfirmed,
	Retry,
	Parked,
	Failed,
};

extern std::atomic<std::size_t> globalQueuedXnm;
extern std::atomic<std::size_t> globalQueuedXuni;
extern std::atomic<LastSubmissionState> globalLastSubmission;
extern std::atomic<treeminer::CircuitBreaker::State> globalNetworkState;
extern std::atomic<std::size_t> globalCpuWorkers;
extern std::atomic<double> globalCpuHashrate;
// The listen address (often 0.0.0.0). Advertised URLs use interface IPs, never this wildcard.
extern std::string globalDashboardBind;
extern int globalDashboardPort;

const char* submissionStateLabel(LastSubmissionState state);
const char* networkStateLabel(treeminer::CircuitBreaker::State state);

bool isWithinXuniWindow();

struct gpuInfo
{
	int index;
	int busId;
	std::string name;
	int memory;
	float usingMemory;
	int temperature;
	float hashrate;
	std::string power;
	size_t hashCount;
	int streamIndex = 0;
};
extern std::map<int, std::pair<gpuInfo, std::chrono::steady_clock::time_point>> globalGpuInfos;
extern std::mutex globalGpuInfosMutex;

// Snapshot of the durable-submission layer for the stats endpoint (PLAN §11.2). Kept as
// plain data so StatReporter/LocalServer need no journal or submitter headers, and so the
// stats path never shares fate with the submitter thread.
struct TreeminerStats {
	int difficulty = 0;             // last observed network difficulty
	int margin_in_effect = 0;       // KiB of headroom currently baked into new hashes
	int effective_difficulty = 0;   // difficulty + margin: what new hashes actually cost
	const char* margin_mode = "off";
	const char* breaker_state = "closed";
	long long outage_ms = 0;        // 0 unless the /verify path is currently open
	double drain_rate_per_second = 0.0;
	std::size_t pending = 0, parked = 0, quarantined = 0;
	std::size_t acked_total = 0, dead_total = 0;
	std::size_t accepted_unconfirmed = 0, permanently_invalid = 0;
};
// Set by main() once the journal and submitter exist; empty until then (the stats endpoint
// starts before mining does). Returns false when the submission layer is not running.
extern std::function<bool(TreeminerStats&)> globalTreeminerStatsProvider;

// Lightweight, in-memory counters for the live terminal status line. Unlike the web
// snapshot above, this deliberately never queries the durable journal: the mining callback
// can refresh it frequently without introducing disk I/O into the batch-completion path.
struct SubmissionLineStats {
	std::uint64_t submitted = 0;
	std::uint64_t resubmitted = 0;
	std::uint64_t confirmed = 0;
	std::uint64_t accepted_unconfirmed = 0;
	std::uint64_t transport_failures = 0;
	bool pool_down = false;
	bool breaker_open = false;
	bool breaker_half_open = false;
	long long outage_ms = 0;
	int margin_kib = 0;
};
extern std::function<bool(SubmissionLineStats&)> globalSubmissionLineStatsProvider;

// `source` identifies the producing backend ("GPU"/"CPU") for per-backend stats.
using SubmitCallback = std::function<void(const std::string& hexsalt, const std::string& key, const std::string& hashed_pure, const std::uint32_t memory_cost, const size_t attempts, const float hashrate, const std::string& source)>;
using StatCallback = std::function<void(const gpuInfo gpuinfo)>;

struct MinerConfig {
	std::string userAddress;
	std::string devfeeAddress;
	std::string ecoDevfeeAddress;
	std::atomic<int> devfeePermillage{0};
	std::string rpcLink;
	std::string testBlockPattern;
	std::string selfMiningPrefix;
	std::size_t maxBatchSize = 0;
	std::string customName;
	bool platformMode = false;
	std::string mqttBroker;
	std::string workerId;
};

const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string RESET = "\033[0m";
