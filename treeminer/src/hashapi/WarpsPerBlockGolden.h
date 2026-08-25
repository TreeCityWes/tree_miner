#pragma once

#include <cstdint>
#include <string>

namespace hashapi {

struct WarpsGoldenResult {
    bool ok = false;
    std::string error;
    std::size_t jobs = 0;
    std::uint32_t warps_per_block = 1;
};

// Same host-prepared first-blocks, oneshot at warps=1 vs warps=N.
// Digests (final Argon2 blocks) must match bit-exact. warps<=1 is a no-op success.
// Requires a CUDA device. Does not open the find journal.
WarpsGoldenResult runWarpsPerBlockGolden(int device_id, std::uint32_t warps_per_block);

} // namespace hashapi
