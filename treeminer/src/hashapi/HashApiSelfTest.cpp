#include "HashApiSelfTest.h"

#include <cstdint>

namespace hashapi {
namespace {

constexpr std::uint32_t kSelfTestDifficulty = 8;
constexpr char kSelfTestSalt[] = "e4bb184781bbc9c7004e8dafd4a9b49d203bc9bc";
constexpr char kSelfTestKey[] =
    "52a13632690c0d5a7e528c91c8462f9d68d24975d4f80cc64d20504063f3590f";

std::string phcDigest(const std::string& phc)
{
    const std::size_t separator = phc.rfind('$');
    if (separator == std::string::npos || separator + 1 >= phc.size()) return {};
    return phc.substr(separator + 1);
}

HashApiRequest requestFor(const char* backend, int device_id, bool gpu_first_blocks)
{
    HashApiRequest request;
    request.backend = backend;
    request.salt_hex = kSelfTestSalt;
    request.key = kSelfTestKey;
    request.target_pattern = "SELFTEST-NO-MATCH";
    request.difficulty = kSelfTestDifficulty;
    request.batch_size = 1;
    request.device_id = device_id;
    request.allow_xuni = false;
    request.gpu_first_blocks = gpu_first_blocks;
    return request;
}

} // namespace

HashApiSelfTestResult runCpuCudaSelfTest(IHashBackend& cpu,
                                         IHashBackend& cuda,
                                         int device_id,
                                         bool gpu_first_blocks)
{
    const HashApiResult cpu_result = cpu.runBatch(requestFor("cpu", device_id, false));
    if (!cpu_result.ok) return {false, "CPU reference failed: " + cpu_result.error};

    const std::string expected_digest = phcDigest(cpu_result.hash);
    if (expected_digest.empty()) return {false, "CPU reference returned an invalid PHC string"};

    const HashApiResult cuda_result =
        cuda.runBatch(requestFor("cuda", device_id, gpu_first_blocks));
    if (!cuda_result.ok) return {false, "CUDA computation failed: " + cuda_result.error};
    if (cuda_result.hash.empty()) return {false, "CUDA computation returned an empty digest"};
    if (cuda_result.hash != expected_digest) {
        return {false,
                "CPU/CUDA Argon2 digest mismatch (gpu_first_blocks=" +
                    std::string(gpu_first_blocks ? "true" : "false") + ")"};
    }
    return {true, {}};
}

} // namespace hashapi
