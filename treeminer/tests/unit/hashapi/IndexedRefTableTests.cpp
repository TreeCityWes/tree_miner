#include "hashapi/IndexedRefTable.h"
#include "hashapi/CudaSkip.h"
#include "hashapi/HashApiValidation.h"
#include "hashapi/HashApiTypes.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

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
        require(hashapi::segmentBlocksForDifficulty(8) == 2, "m=8 → 2 segment blocks");
        require(hashapi::segmentBlocksForDifficulty(4096) == 1024, "m=4096 → 1024");
        require(hashapi::indexedRefCount(2) == 2, "m=8 table has 2 refs");
        require(hashapi::indexedRefCount(1) == 0, "too small");
        require(hashapi::indexedRefIndex(0, 2, 8) == 0, "slice0 offset2");
        require(hashapi::indexedRefIndex(1, 0, 8) == 6, "slice1 offset0 after 6 slice0 slots");

        require(hashapi::kDefaultPrecomputedRefs == false, "live default is off");

        const auto empty = hashapi::generateIndexedRefTable(1);
        require(empty.empty(), "segment_blocks<2 is empty");

        const auto m8 = hashapi::generateIndexedRefTable(2);
        require(m8.size() == 2, "m=8 size");
        require(m8[0] == 0, "slice1 offset0 ref_area=1 → index 0");
        require(m8[1] <= 1, "slice1 offset1 ref_area=2 → {0,1}");

        const auto m8b = hashapi::generateIndexedRefTable(2);
        require(m8 == m8b, "deterministic");

        const auto m32 = hashapi::generateIndexedRefTable(8);
        require(m32.size() == hashapi::indexedRefCount(8), "m=32 count");
        require(m32[0] == 0, "slice0 offset2 always refs block 0");
        require(m32[1] <= 1, "slice0 offset3 in {0,1}");
        for (std::uint32_t offset = 2; offset < 8; ++offset) {
            const std::size_t i = hashapi::indexedRefIndex(0, offset, 8);
            const std::uint32_t area = offset - 1;
            require(m32[i] < area, "slice0 ref in range");
        }
        for (std::uint32_t offset = 0; offset < 8; ++offset) {
            const std::size_t i = hashapi::indexedRefIndex(1, offset, 8);
            const std::uint32_t area = 8 + offset - 1;
            require(m32[i] < area, "slice1 ref in range");
        }

        const auto m64 = hashapi::generateIndexedRefTable(16);
        require(m64 != m32, "different m → different table");
        require(m64.size() == 30, "2*16-2");

        std::unordered_set<std::uint32_t> distinct(m64.begin(), m64.end());
        require(distinct.size() > 1, "not a constant stream");

        require(hashapi::computeRefPos(8, 0, 2, 0xffffffffu) == 0, "rfc ref_pos at first indexed");

        require(hashapi::shouldSkipDevice(hashapi::kCudaErrorNoKernelImageForDevice), "skip 209");
        require(hashapi::shouldSkipDevice(hashapi::kCudaErrorInvalidPtx), "skip 218");
        require(!hashapi::shouldSkipDevice(0), "success is not skip");
        require(!hashapi::shouldSkipDevice(700), "unrelated error is not skip");
        const std::string skip_log = hashapi::skipDeviceLog(3, hashapi::kCudaErrorNoKernelImageForDevice);
        require(skip_log.find("GPU #3") != std::string::npos, "skip log names device");
        require(skip_log.find("fat binary") != std::string::npos, "skip log names fat binary");

        hashapi::HashApiRequest cuda;
        cuda.algorithm = "argon2id-xen";
        cuda.backend = "cuda";
        cuda.salt_hex = "aabbccddeeff0011";
        cuda.target_pattern = "XEN11";
        cuda.difficulty = 8;
        cuda.batch_size = 8;
        cuda.precomputed_refs = true;
        require(hashapi::isValidRequest(cuda), "cuda precomputed_refs is valid");

        hashapi::HashApiRequest cpu = cuda;
        cpu.backend = "cpu";
        const auto cpu_errors = hashapi::validateRequest(cpu);
        require(!cpu_errors.empty(), "cpu precomputed_refs rejected");
        require(hashapi::joinErrors(cpu_errors).find("precomputed_refs requires backend=cuda") !=
                    std::string::npos,
                "cpu precomputed_refs error string");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
