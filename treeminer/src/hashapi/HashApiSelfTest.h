#pragma once

#include "HashApiTypes.h"

#include <string>

namespace hashapi {

struct HashApiSelfTestResult {
    bool ok = false;
    std::string error;
};

HashApiSelfTestResult runCpuCudaSelfTest(IHashBackend& cpu,
                                         IHashBackend& cuda,
                                         int device_id,
                                         bool gpu_first_blocks = kGpuFirstBlocksEnabled);

} // namespace hashapi
