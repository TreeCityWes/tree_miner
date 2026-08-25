#include "hashapi/OneshotLaunch.h"
#include "hashapi/HashApiValidation.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
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
        const auto one = hashapi::makeOneshotLaunch(128, 1);
        require(one.warps_per_block == 1, "default warps");
        require(one.grid == 128, "one block per hash");
        require(one.threads == 32, "one warp of 32");
        require(one.shared_bytes == 1024, "1 KiB shared");
        require(hashapi::oneshotJobId(7, 0, 1) == 7, "job id is block index");
        require(hashapi::oneshotLane(31) == 31, "lane is thread in warp");

        const auto four = hashapi::makeOneshotLaunch(8, 4);
        require(four.grid == 2, "8 hashes / 4 warps = 2 blocks");
        require(four.threads == 128, "4 warps * 32");
        require(four.shared_bytes == 4096, "4 KiB shared");
        require(hashapi::oneshotJobId(1, 32, 4) == 5, "block1 warp1 -> job 5");
        require(hashapi::oneshotLane(32) == 0, "second warp lane 0");
        require(hashapi::oneshotJobId(1, 127, 4) == 7, "last thread of block is job 7");

        const auto partial = hashapi::makeOneshotLaunch(7, 4);
        require(partial.grid == 2, "ceil(7/4) blocks");
        require(hashapi::oneshotJobId(1, 64, 4) == 6, "block1 warp2 is last in-range job");
        require(hashapi::oneshotJobId(1, 96, 4) == 7, "block1 warp3 is idle for batch=7");

        require(hashapi::resolveWarpsPerBlock(0) == 1, "0 means default");
        require(hashapi::resolveWarpsPerBlock(4) == 4, "in range");
        require(hashapi::resolveWarpsPerBlock(17) == 0, "over max is invalid");

        hashapi::HashApiRequest cuda;
        cuda.algorithm = "argon2id-xen";
        cuda.backend = "cuda";
        cuda.salt_hex = "aabbccddeeff0011";
        cuda.target_pattern = "XEN11";
        cuda.difficulty = 8;
        cuda.batch_size = 8;
        cuda.warps_per_block = 4;
        require(hashapi::isValidRequest(cuda), "cuda warps=4 is valid");

        hashapi::HashApiRequest cpu = cuda;
        cpu.backend = "cpu";
        cpu.warps_per_block = 4;
        const auto cpu_errors = hashapi::validateRequest(cpu);
        require(!cpu_errors.empty(), "cpu warps=4 rejected");
        require(hashapi::joinErrors(cpu_errors).find("warps_per_block > 1 requires backend=cuda") !=
                    std::string::npos,
                "cpu warps error string");

        cuda.warps_per_block = 99;
        const auto max_errors = hashapi::validateRequest(cuda);
        require(hashapi::joinErrors(max_errors).find("warps_per_block exceeds maximum of 16") !=
                    std::string::npos,
                "max warps error string");

        hashapi::HashApiRequest cpu_default = cpu;
        cpu_default.warps_per_block = 0;
        require(hashapi::isValidRequest(cpu_default), "cpu default warps is valid");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
