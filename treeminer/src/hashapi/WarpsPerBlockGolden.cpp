#include "WarpsPerBlockGolden.h"

#include "OneshotLaunch.h"
#include "../argon2-common.h"
#include "../argon2params.h"
#include "../CudaException.h"
#include "../kernelrunner.h"

#include "../gpu/GpuRuntime.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace hashapi {
namespace {

constexpr std::size_t kGoldenJobs = 8;
constexpr std::uint32_t kGoldenDifficulty = 8;
constexpr std::size_t kHashLength = 64;
constexpr char kGoldenSalt[] = "e4bb184781bbc9c7004e8dafd4a9b49d203bc9bc";

std::string keyForJob(std::size_t job)
{
    // Distinct 64-hex passwords so two warps in one block cannot share a coincidence.
    std::string key(64, 'a');
    key[62] = "0123456789abcdef"[job / 16];
    key[63] = "0123456789abcdef"[job % 16];
    return key;
}

void fillInputs(KernelRunner& runner, const Argon2Params& params)
{
    for (std::size_t job = 0; job < kGoldenJobs; ++job) {
        const std::string key = keyForJob(job);
        params.fillFirstBlocks(runner.getInputMemory(job), key.c_str(), key.size());
    }
}

std::vector<std::uint8_t> captureOutputs(const KernelRunner& runner)
{
    std::vector<std::uint8_t> out(kGoldenJobs * argon2::ARGON2_BLOCK_SIZE);
    for (std::size_t job = 0; job < kGoldenJobs; ++job) {
        std::memcpy(out.data() + job * argon2::ARGON2_BLOCK_SIZE,
                    runner.getOutputMemory(job),
                    argon2::ARGON2_BLOCK_SIZE);
    }
    return out;
}

} // namespace

WarpsGoldenResult runWarpsPerBlockGolden(int device_id, std::uint32_t warps_per_block)
{
    WarpsGoldenResult result;
    result.warps_per_block = resolveWarpsPerBlock(warps_per_block);
    result.jobs = kGoldenJobs;
    if (result.warps_per_block <= 1) {
        result.ok = true;
        return result;
    }
    if (result.warps_per_block == 0) {
        result.error = "warps_per_block exceeds maximum of 16";
        return result;
    }

    try {
        CudaException::check(cudaSetDevice(device_id));
        Argon2Params params(argon2::ARGON2_ID, argon2::ARGON2_VERSION_13,
                            kHashLength, kGoldenSalt, nullptr, 0, nullptr, 0,
                            1, kGoldenDifficulty, 1);
        KernelRunner runner(argon2::ARGON2_ID, argon2::ARGON2_VERSION_13,
                            1, 1, params.getSegmentBlocks(), kGoldenJobs);
        runner.init(kGoldenJobs);
        fillInputs(runner, params);

        runner.setWarpsPerBlock(kDefaultWarpsPerBlock);
        runner.run();
        runner.finish();
        const auto baseline = captureOutputs(runner);

        fillInputs(runner, params);
        runner.setWarpsPerBlock(result.warps_per_block);
        runner.run();
        runner.finish();
        const auto packed = captureOutputs(runner);

        if (baseline != packed) {
            result.error = "oneshot output mismatch between warps_per_block=1 and warps_per_block=" +
                           std::to_string(result.warps_per_block);
            return result;
        }
        result.ok = true;
        return result;
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }
}

} // namespace hashapi
