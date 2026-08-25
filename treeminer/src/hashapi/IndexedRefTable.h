#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hashapi {

// Argon2id oneshot uses data-independent addressing for slices 0 and 1 (the
// "indexed half"). The address-generation input has no job-specific field:
// at fixed difficulty the ref-index stream is identical for every hash.
// This table is the CPU-side golden of that stream and the enabler for a
// later cp.async prefetch pipeline. Live mining does not consume it until
// --precomputedRefs is on and a GPU golden matches.

constexpr bool kDefaultPrecomputedRefs = false;

inline std::uint32_t segmentBlocksForDifficulty(std::uint32_t memory_cost,
                                                std::uint32_t lanes = 1)
{
    const std::uint32_t segments = lanes * 4; // ARGON2_SYNC_POINTS
    if (segments == 0) {
        return 0;
    }
    const std::uint32_t min_blocks = 2 * segments;
    const std::uint32_t blocks = memory_cost < min_blocks ? min_blocks : memory_cost;
    return blocks / segments;
}

inline std::size_t indexedRefCount(std::uint32_t segment_blocks)
{
    if (segment_blocks < 2) {
        return 0;
    }
    // slice 0: offsets 2 .. segment_blocks-1; slice 1: offsets 0 .. segment_blocks-1
    return static_cast<std::size_t>(2 * segment_blocks - 2);
}

inline std::size_t indexedRefIndex(std::uint32_t slice,
                                   std::uint32_t offset,
                                   std::uint32_t segment_blocks)
{
    if (slice == 0) {
        return static_cast<std::size_t>(offset - 2);
    }
    return static_cast<std::size_t>((segment_blocks - 2) + offset);
}

// Bit-exact host clone of kernelrunner.cu next_addresses1 + compute_ref_pos
// for the indexed half (slices 0 and 1). Empty if segment_blocks < 2.
std::vector<std::uint32_t> generateIndexedRefTable(std::uint32_t segment_blocks);

inline std::uint32_t computeRefPos(std::uint32_t segment_blocks,
                                   std::uint32_t slice,
                                   std::uint32_t offset,
                                   std::uint32_t ref_index)
{
    const std::uint32_t ref_area_size = slice * segment_blocks + offset - 1;
    const std::uint64_t sq = static_cast<std::uint64_t>(ref_index) * static_cast<std::uint64_t>(ref_index);
    const std::uint32_t index = static_cast<std::uint32_t>(sq >> 32);
    const std::uint64_t area_x = static_cast<std::uint64_t>(ref_area_size) * static_cast<std::uint64_t>(index);
    return ref_area_size - 1 - static_cast<std::uint32_t>(area_x >> 32);
}

} // namespace hashapi
