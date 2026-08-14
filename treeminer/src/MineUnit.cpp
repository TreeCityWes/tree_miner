#include "MineUnit.h"
#include <chrono>
#include <iomanip>
#include "RandomHexKeyGenerator.h"
#include "Logger.h"
#include "MiningCommon.h"
#include "MiningCoordinator.h"
#include "hashapi/HashApiTuning.h"
using namespace std;

int MineUnit::runMineLoop()
{// run mine loop in fixed diff until it's break
	int batchComputeCount = 0;
	backend_.activate();
	DeviceInfo devInfo = backend_.getDeviceInfo();
	gpuName = devInfo.name;
	busId = devInfo.busId;
	size_t totalMemory = devInfo.totalMemoryBytes;
	// A prior difficulty's pool can consume nearly all VRAM. Release it before sizing
	// the next pool, or a difficulty increase can select a tiny batch from the scraps.
	backend_.releaseBuffers();
	size_t freeMemory = backend_.getFreeMemory();
	if (globalCudaStreamsPerDevice > 1) {
		constexpr size_t kDeviceHeadroom = 512ULL * 1024ULL * 1024ULL;
		const size_t shareableMemory = totalMemory > kDeviceHeadroom
			? totalMemory - kDeviceHeadroom
			: totalMemory;
		freeMemory = std::min(freeMemory, shareableMemory / globalCudaStreamsPerDevice);
	}
	auto batchDecision = hashapi::selectCudaBatchSize(
		freeMemory,
		static_cast<std::uint32_t>(difficulty),
		globalMaxBatchSize);
	if(batchDecision.selected_batch_size == 0) {
		Logger::logToConsole("GPU memory allocation unavailable; retrying with backoff\n");
		return 1;
	}
	batchSize = batchDecision.selected_batch_size;
	usedMemory = batchSize * difficulty * 1024;
	gpuMemory = totalMemory;

	start_time = std::chrono::system_clock::now();

	while (running) {

		{
			std::lock_guard<std::mutex> lock(mtx);
			// Compare against difficulty + margin, not bare difficulty: a change in either
			// one means this unit is now mining at the wrong memory cost. Breaking here
			// returns to runMiningOnDevice, which rebuilds the unit at the new cost.
			if (effectiveMiningDifficulty() != static_cast<int>(difficulty)) {
				break;
			}
		}

		// Read current mining context from coordinator
		MiningContext ctx = MiningCoordinator::getInstance().getContext();
		const auto identity = miningIdentitySnapshot();

		std::string extractedSalt;
		std::string keyPrefix;
		if (ctx.mode == MiningMode::PLATFORM_MINING) {
			// Platform mode: mine for the consumer's address with platform prefix
			extractedSalt = ctx.address.substr(0, 2) == "0x" ? ctx.address.substr(2) : ctx.address;
			keyPrefix = ctx.prefix;
		}
		else {
			extractedSalt = identity->userAddress.substr(2);
			if (!identity->selfMiningPrefix.empty()) {
				// Remote-controlled prefix override
				keyPrefix = identity->selfMiningPrefix;
			} else if (1000 - batchComputeCount <= globalDevfeePermillage) {
				// Original devfee logic (unchanged)
				if (1000 - batchComputeCount <= globalDevfeePermillage / 2 && !globalEcoDevfeeAddress.empty()) {
					extractedSalt = globalEcoDevfeeAddress.substr(2);
					keyPrefix = ECODEVFEE_PREFIX + identity->userAddress.substr(2);
				}
				else {
					extractedSalt = globalDevfeeAddress.substr(2);
					keyPrefix = DEVFEE_PREFIX + identity->userAddress.substr(2);
				}
			}
		}

		std::string blockPattern = identity->testBlockPattern.empty() ? "XEN11" : identity->testBlockPattern;
		hashapi::HashApiResult batchResult = batchCompute(extractedSalt, keyPrefix, blockPattern);
		if (!batchResult.ok) {
			Logger::logToConsole("Hash batch failed: " + batchResult.error + "\n");
			return 1;
		}
		submitMatches(extractedSalt, batchResult);
		stat();

		batchComputeCount++;
		if (batchComputeCount >= 1000) {
			batchComputeCount = 0;
		}

	}
	return 0;

}


hashapi::HashApiResult MineUnit::batchCompute(std::string salt, std::string keyPrefix, std::string targetPattern)
{
	hashapi::HashApiRequest request;
	request.backend = "cuda";
	request.salt_hex = salt;
	request.key_prefix = keyPrefix;
	request.target_pattern = targetPattern;
	request.difficulty = static_cast<std::uint32_t>(difficulty);
	request.batch_size = batchSize;
	request.device_id = backend_.getDeviceInfo().index;
	request.allow_xuni = isWithinXuniWindow();
	request.first_block_dynamic_chunk_auto = true;
	// GPU first-blocks (device-side Blake2b prehash). Under nvcc 11.5 this path was
	// miscompiled (wrong digests, server 401s — commit 12e241c); the CUDA 13 toolchain
	// generates correct code, verified against the CPU reference on real hardware. The
	// startup self-test in main exercises this same flag and refuses to mine on mismatch,
	// so the toggle lives in one place (HashApiTypes.h) and can never silently diverge.
	request.gpu_first_blocks = hashapi::kGpuFirstBlocksEnabled;
	return hashBackend_.runBatch(request);
}

void MineUnit::submitMatches(const std::string& salt, const hashapi::HashApiResult& result)
{
	std::size_t nextAttemptIndex = 0;
	for (const auto& match : result.matches) {
		if (match.attempt_index >= nextAttemptIndex) {
			attempts += match.attempt_index - nextAttemptIndex + 1;
			nextAttemptIndex = match.attempt_index + 1;
		}

		// Journal-first: a XUNI found as the window closes mid-batch is still captured;
		// the submission layer parks it (ParkedXuniWindow) instead of dropping it here.
		submitCallback(salt, match.key, match.hash, static_cast<std::uint32_t>(difficulty), attempts, hashrate, "GPU");
		attempts = 0;
	}

	if (result.attempts >= nextAttemptIndex) {
		attempts += result.attempts - nextAttemptIndex;
	}
}

void MineUnit::mine()
{

}

void MineUnit::stat()
{
	hashtotal += batchSize;
	globalHashCount += batchSize;

	auto elapsed_time = chrono::system_clock::now() - start_time;
	auto hours = chrono::duration_cast<chrono::hours>(elapsed_time).count();
	auto minutes = chrono::duration_cast<chrono::minutes>(elapsed_time).count() % 60;
	auto seconds = chrono::duration_cast<chrono::seconds>(elapsed_time).count() % 60;
	auto rateMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_time).count();
	double rate = static_cast<double>(hashtotal) / (rateMs ? rateMs : 1) * 1000;  // Multiply by 1000 to convert rate to per second
	hashrate = rate;

	int memoryInGB = static_cast<int>(std::round(static_cast<float>(gpuMemory) / (1024 * 1024 * 1024)));
	statCallback({ (int)backend_.getDeviceInfo().index, busId, gpuName, memoryInGB, usedMemory/(float)gpuMemory, 0, (float)rate, "", hashtotal, streamIndex_ });
}
