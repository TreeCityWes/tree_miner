#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hashapi {

constexpr std::size_t kCudaBatchMemoryReserveBytes = 100ULL * 1024ULL * 1024ULL;

struct CudaBatchSizeDecision {
    std::size_t memory_limited_batch_size = 0;
    std::size_t tuned_batch_size = 0;
    std::size_t selected_batch_size = 0;
    bool explicit_limit_applied = false;
    bool tuned_default_applied = false;
};

std::size_t estimateCudaMemoryBatchLimit(std::size_t free_memory_bytes,
                                         std::uint32_t difficulty,
                                         std::size_t reserve_bytes = kCudaBatchMemoryReserveBytes);

std::size_t recommendedCudaBatchSize(std::uint32_t difficulty);

std::size_t recommendedCudaBatchSizeForDifficultySequence(
    const std::vector<std::uint32_t>& difficulties);

CudaBatchSizeDecision selectCudaBatchSize(std::size_t free_memory_bytes,
                                          std::uint32_t difficulty,
                                          std::size_t explicit_max_batch_size);

CudaBatchSizeDecision selectCudaBatchSizeForDifficultySequence(
    std::size_t free_memory_bytes,
    const std::vector<std::uint32_t>& difficulties,
    std::size_t explicit_max_batch_size);

// Fair VRAM share for one stream among the sibling streams of a single device.
// device_free_bytes is cudaMemGetInfo "free" observed AFTER the caller released its own
// pool; sibling_committed_bytes are pools siblings actually hold (they would be free if
// released, so they belong to the shared pool); sibling_pending_bytes are pools siblings
// have reserved but not yet allocated (they still appear in "free" and must not be
// promised twice). Memory held by other processes is in neither term and is therefore
// excluded from the pool by construction.
struct StreamShareInput {
    std::size_t device_free_bytes = 0;
    std::size_t sibling_committed_bytes = 0;
    std::size_t sibling_pending_bytes = 0;
    std::size_t active_streams = 1;
    std::size_t headroom_bytes = 0;
};

// min((free + committed - headroom) / streams, free - pending). Returns 0 when the pool
// cannot cover the headroom or when siblings' pending reservations exhaust current free
// memory (the caller backs off and retries).
std::size_t computeStreamMemoryShare(const StreamShareInput& in);

} // namespace hashapi
