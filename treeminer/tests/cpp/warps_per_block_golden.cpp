#include "hashapi/WarpsPerBlockGolden.h"

#include "gpu/GpuRuntime.h"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    int device = 0;
    std::uint32_t warps = 4;
    if (argc > 1) {
        warps = static_cast<std::uint32_t>(std::stoul(argv[1]));
    }
    if (argc > 2) {
        device = std::stoi(argv[2]);
    }

    int count = 0;
    const cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess || count <= 0) {
        std::cout << "SKIP: no " TREEMINER_GPU_BACKEND_NAME " device (" << cudaGetErrorString(status) << ")\n";
        return 0;
    }
    if (device < 0 || device >= count) {
        std::cerr << "device index out of range\n";
        return 1;
    }

    const auto result = hashapi::runWarpsPerBlockGolden(device, warps);
    if (!result.ok) {
        std::cerr << "warps-per-block golden failed: " << result.error << '\n';
        return 1;
    }
    std::cout << "ok jobs=" << result.jobs
              << " warps_per_block=" << result.warps_per_block << '\n';
    return 0;
}
