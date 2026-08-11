#include "MiningCommon.h"

#include <ctime>

std::atomic<bool> running = true;
std::atomic<int> globalDifficulty = 1727;
std::mutex mtx;
std::mutex coutmtx;

std::string globalUserAddress = "0x123456789";
std::string globalDevfeeAddress = "0x24691E54aFafe2416a8252097C9Ca67557271475";
std::string globalEcoDevfeeAddress = "";
std::atomic<int> globalDevfeePermillage = 1; // per 1000
std::string machineId = "00000";

std::map<int, std::pair<gpuInfo, std::chrono::steady_clock::time_point>> globalGpuInfos;
std::mutex globalGpuInfosMutex;

std::atomic<int> globalNormalBlockCount = 0;
std::atomic<int> globalSuperBlockCount = 0;
std::atomic<int> globalXuniBlockCount = 0;
std::atomic<int> globalFailedBlockCount = 0;

std::atomic<long> globalHashCount = 0;
std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();

std::string globalRpcLink = "http://xenblocks.io";
std::string globalSelfMiningPrefix;
std::size_t globalMaxBatchSize = 0; // 0 = auto (use all free GPU memory)
std::size_t globalCudaStreamsPerDevice = 1;
std::atomic<std::size_t> globalQueuedXnm{0};
std::atomic<std::size_t> globalQueuedXuni{0};
std::atomic<LastSubmissionState> globalLastSubmission{LastSubmissionState::None};
std::atomic<treeminer::CircuitBreaker::State> globalNetworkState{
	treeminer::CircuitBreaker::State::Closed};
std::atomic<std::size_t> globalCpuWorkers{0};
std::atomic<double> globalCpuHashrate{0.0};

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
