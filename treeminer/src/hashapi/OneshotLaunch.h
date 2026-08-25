#pragma once

#include <cstddef>
#include <cstdint>

namespace hashapi {

// Launch geometry for argon2_kernel_oneshot.
// Default is the Woody shape: one hash per CUDA block, one warp (32 threads),
// 1 KiB dynamic shared. warps_per_block > 1 packs independent hashes into the
// same block (PLAN Phase 3 occupancy). Live mining stays at 1 until a GPU
// canary + Nsight dump says otherwise.
constexpr std::uint32_t kThreadsPerLane = 32;
constexpr std::uint32_t kDefaultWarpsPerBlock = 1;
constexpr std::uint32_t kMaxWarpsPerBlock = 16;
constexpr std::size_t kOneshotSharedBytesPerWarp = 1024; // sizeof(block_l)

inline std::uint32_t resolveWarpsPerBlock(std::size_t requested)
{
    if (requested == 0) {
        return kDefaultWarpsPerBlock;
    }
    if (requested > kMaxWarpsPerBlock) {
        return 0;
    }
    return static_cast<std::uint32_t>(requested);
}

struct OneshotLaunch {
    std::uint32_t warps_per_block = kDefaultWarpsPerBlock;
    std::uint32_t grid = 0;
    std::uint32_t threads = 0;
    std::uint32_t shared_bytes = 0;
};

inline OneshotLaunch makeOneshotLaunch(std::size_t batch_size, std::uint32_t warps_per_block)
{
    OneshotLaunch launch;
    const std::uint32_t warps =
        warps_per_block == 0 ? kDefaultWarpsPerBlock : warps_per_block;
    launch.warps_per_block = warps;
    if (batch_size == 0 || warps == 0) {
        return launch;
    }
    launch.grid = static_cast<std::uint32_t>((batch_size + warps - 1) / warps);
    launch.threads = warps * kThreadsPerLane;
    launch.shared_bytes = static_cast<std::uint32_t>(warps * kOneshotSharedBytesPerWarp);
    return launch;
}

inline std::size_t oneshotJobId(std::uint32_t block_idx,
                                std::uint32_t thread_idx,
                                std::uint32_t warps_per_block)
{
    const std::uint32_t warps =
        warps_per_block == 0 ? kDefaultWarpsPerBlock : warps_per_block;
    const std::uint32_t warp = thread_idx / kThreadsPerLane;
    return static_cast<std::size_t>(block_idx) * warps + warp;
}

inline std::uint32_t oneshotLane(std::uint32_t thread_idx)
{
    return thread_idx % kThreadsPerLane;
}

} // namespace hashapi
