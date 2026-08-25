#pragma once

#include <string>

namespace hashapi {

// Numeric CUDA runtime codes from cuda_runtime_api.h so GPU-free tests compile
// without the toolkit headers.
constexpr int kCudaErrorNoKernelImageForDevice = 209;
constexpr int kCudaErrorInvalidPtx = 218;
constexpr int kCudaErrorUnsupportedPtxVersion = 222;
constexpr int kCudaErrorInvalidKernelImage = 300;

inline bool shouldSkipDevice(int cuda_error)
{
    return cuda_error == kCudaErrorNoKernelImageForDevice ||
           cuda_error == kCudaErrorInvalidPtx ||
           cuda_error == kCudaErrorUnsupportedPtxVersion ||
           cuda_error == kCudaErrorInvalidKernelImage;
}

inline const char* skipDeviceReason(int cuda_error)
{
    switch (cuda_error) {
    case kCudaErrorNoKernelImageForDevice:
        return "no kernel image for this compute capability (rebuild a fat binary)";
    case kCudaErrorInvalidPtx:
        return "invalid PTX for this driver/toolkit";
    case kCudaErrorUnsupportedPtxVersion:
        return "PTX version unsupported by this driver";
    case kCudaErrorInvalidKernelImage:
        return "invalid kernel image for this device";
    default:
        return "CUDA device is unusable";
    }
}

inline std::string skipDeviceLog(int device_index, int cuda_error)
{
    return "GPU #" + std::to_string(device_index) + " skipped: " +
           skipDeviceReason(cuda_error);
}

} // namespace hashapi
