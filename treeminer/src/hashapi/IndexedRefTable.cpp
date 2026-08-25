#include "IndexedRefTable.h"

#include <array>
#include <cstdint>

namespace hashapi {
namespace {

constexpr std::uint32_t kThreadsPerLane = 32;
constexpr std::uint32_t kQwordsInBlock = 128;
constexpr std::uint32_t kSyncPoints = 4;

struct Lane {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
};

using Warp = std::array<Lane, kThreadsPerLane>;

struct BlockL {
    std::uint32_t lo[kQwordsInBlock]{};
    std::uint32_t hi[kQwordsInBlock]{};
};

inline std::uint32_t u64_lo(std::uint64_t x)
{
    return static_cast<std::uint32_t>(x);
}

inline std::uint32_t u64_hi(std::uint64_t x)
{
    return static_cast<std::uint32_t>(x >> 32);
}

inline std::uint64_t u64_build(std::uint32_t hi, std::uint32_t lo)
{
    return (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
}

inline std::uint64_t rotr64(std::uint64_t x, std::uint32_t n)
{
    return (x >> n) | (x << (64 - n));
}

inline std::uint64_t f(std::uint64_t x, std::uint64_t y)
{
    const std::uint32_t xlo = u64_lo(x);
    const std::uint32_t ylo = u64_lo(y);
    const std::uint32_t hi = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(xlo) * static_cast<std::uint64_t>(ylo)) >> 32);
    const std::uint32_t lo = xlo * ylo;
    return x + y + 2 * u64_build(hi, lo);
}

inline void g(Lane& block)
{
    std::uint64_t a = block.a;
    std::uint64_t b = block.b;
    std::uint64_t c = block.c;
    std::uint64_t d = block.d;

    a = f(a, b);
    d = rotr64(d ^ a, 32);
    c = f(c, d);
    b = rotr64(b ^ c, 24);
    a = f(a, b);
    d = rotr64(d ^ a, 16);
    c = f(c, d);
    b = rotr64(b ^ c, 63);

    block.a = a;
    block.b = b;
    block.c = c;
    block.d = d;
}

inline std::uint64_t lane_get(const Lane& b, std::uint32_t idx)
{
    switch (idx) {
    case 0:
        return b.a;
    case 1:
        return b.b;
    case 2:
        return b.c;
    default:
        return b.d;
    }
}

inline void transpose(Warp& warp)
{
    Warp snap = warp;
    std::array<std::uint64_t, kThreadsPerLane> x1{};
    std::array<std::uint64_t, kThreadsPerLane> x2{};
    std::array<std::uint64_t, kThreadsPerLane> x3{};
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const std::uint32_t g1f = (thread & 0x4);
        const std::uint32_t g2f = (thread & 0x8);
        const Lane& block = snap[thread];
        x1[thread] = (g2f ? (g1f ? block.c : block.d) : (g1f ? block.a : block.b));
        x2[thread] = (g2f ? (g1f ? block.b : block.a) : (g1f ? block.d : block.c));
        x3[thread] = (g2f ? (g1f ? block.a : block.b) : (g1f ? block.c : block.d));
    }
    std::array<std::uint64_t, kThreadsPerLane> y1{};
    std::array<std::uint64_t, kThreadsPerLane> y2{};
    std::array<std::uint64_t, kThreadsPerLane> y3{};
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        y1[thread] = x1[thread ^ 0x4];
        y2[thread] = x2[thread ^ 0x8];
        y3[thread] = x3[thread ^ 0xC];
    }
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const std::uint32_t g1f = (thread & 0x4);
        const std::uint32_t g2f = (thread & 0x8);
        Lane& block = warp[thread];
        const Lane& orig = snap[thread];
        block.a = (g2f ? (g1f ? y3[thread] : y2[thread]) : (g1f ? y1[thread] : orig.a));
        block.b = (g2f ? (g1f ? y2[thread] : y3[thread]) : (g1f ? orig.b : y1[thread]));
        block.c = (g2f ? (g1f ? y1[thread] : orig.c) : (g1f ? y3[thread] : y2[thread]));
        block.d = (g2f ? (g1f ? orig.d : y1[thread]) : (g1f ? y2[thread] : y3[thread]));
    }
}

inline void shift1_shuffle(Warp& warp)
{
    Warp snap = warp;
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const std::uint32_t src_b = (thread & 0x1c) | ((thread + 1) & 0x3);
        const std::uint32_t src_d = (thread & 0x1c) | ((thread + 3) & 0x3);
        warp[thread].b = snap[src_b].b;
        warp[thread].c = snap[thread ^ 0x2].c;
        warp[thread].d = snap[src_d].d;
    }
}

inline void unshift1_shuffle(Warp& warp)
{
    Warp snap = warp;
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const std::uint32_t src_b = (thread & 0x1c) | ((thread + 3) & 0x3);
        const std::uint32_t src_d = (thread & 0x1c) | ((thread + 1) & 0x3);
        warp[thread].b = snap[src_b].b;
        warp[thread].c = snap[thread ^ 0x2].c;
        warp[thread].d = snap[src_d].d;
    }
}

inline void shift2_shuffle(Warp& warp)
{
    Warp snap = warp;
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const std::uint32_t lo = (thread & 0x1) | ((thread & 0x10) >> 3);
        const std::uint32_t src_b = (((lo + 1) & 0x2) << 3) | (thread & 0xe) | ((lo + 1) & 0x1);
        const std::uint32_t src_d = (((lo + 3) & 0x2) << 3) | (thread & 0xe) | ((lo + 3) & 0x1);
        warp[thread].b = snap[src_b].b;
        warp[thread].c = snap[thread ^ 0x10].c;
        warp[thread].d = snap[src_d].d;
    }
}

inline void unshift2_shuffle(Warp& warp)
{
    Warp snap = warp;
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const std::uint32_t lo = (thread & 0x1) | ((thread & 0x10) >> 3);
        const std::uint32_t src_b = (((lo + 3) & 0x2) << 3) | (thread & 0xe) | ((lo + 3) & 0x1);
        const std::uint32_t src_d = (((lo + 1) & 0x2) << 3) | (thread & 0xe) | ((lo + 1) & 0x1);
        warp[thread].b = snap[src_b].b;
        warp[thread].c = snap[thread ^ 0x10].c;
        warp[thread].d = snap[src_d].d;
    }
}

inline void shuffle_block(Warp& warp)
{
    transpose(warp);
    for (auto& lane : warp) {
        g(lane);
    }
    shift1_shuffle(warp);
    for (auto& lane : warp) {
        g(lane);
    }
    unshift1_shuffle(warp);
    transpose(warp);
    for (auto& lane : warp) {
        g(lane);
    }
    shift2_shuffle(warp);
    for (auto& lane : warp) {
        g(lane);
    }
    unshift2_shuffle(warp);
}

inline void block_l_store(BlockL& dst, const Warp& warp)
{
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        const Lane& src = warp[thread];
        dst.lo[0 * kThreadsPerLane + thread] = u64_lo(src.a);
        dst.hi[0 * kThreadsPerLane + thread] = u64_hi(src.a);
        dst.lo[1 * kThreadsPerLane + thread] = u64_lo(src.b);
        dst.hi[1 * kThreadsPerLane + thread] = u64_hi(src.b);
        dst.lo[2 * kThreadsPerLane + thread] = u64_lo(src.c);
        dst.hi[2 * kThreadsPerLane + thread] = u64_hi(src.c);
        dst.lo[3 * kThreadsPerLane + thread] = u64_lo(src.d);
        dst.hi[3 * kThreadsPerLane + thread] = u64_hi(src.d);
    }
}

inline void block_l_load_xor(Warp& warp, const BlockL& src)
{
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        Lane& dst = warp[thread];
        dst.a ^= u64_build(src.hi[0 * kThreadsPerLane + thread],
                           src.lo[0 * kThreadsPerLane + thread]);
        dst.b ^= u64_build(src.hi[1 * kThreadsPerLane + thread],
                           src.lo[1 * kThreadsPerLane + thread]);
        dst.c ^= u64_build(src.hi[2 * kThreadsPerLane + thread],
                           src.lo[2 * kThreadsPerLane + thread]);
        dst.d ^= u64_build(src.hi[3 * kThreadsPerLane + thread],
                           src.lo[3 * kThreadsPerLane + thread]);
    }
}

inline void next_addresses1(Warp& addr, BlockL& tmp, const std::array<std::uint32_t, kThreadsPerLane>& thread_input)
{
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        addr[thread].a = u64_build(0, thread_input[thread]);
        addr[thread].b = 0;
        addr[thread].c = 0;
        addr[thread].d = 0;
    }
    shuffle_block(addr);
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        addr[thread].a ^= u64_build(0, thread_input[thread]);
    }
    block_l_store(tmp, addr);
    shuffle_block(addr);
    block_l_load_xor(addr, tmp);
}

inline std::uint32_t extract_ref_index(const Warp& addr, std::uint32_t addr_index)
{
    const std::uint32_t thr = addr_index % kThreadsPerLane;
    const std::uint32_t idx = addr_index / kThreadsPerLane;
    return u64_lo(lane_get(addr[thr], idx));
}

} // namespace

std::vector<std::uint32_t> generateIndexedRefTable(std::uint32_t segment_blocks)
{
    std::vector<std::uint32_t> table;
    const std::size_t count = indexedRefCount(segment_blocks);
    if (count == 0) {
        return table;
    }
    table.reserve(count);

    const std::uint32_t lane_blocks = kSyncPoints * segment_blocks;
    std::array<std::uint32_t, kThreadsPerLane> thread_input{};
    for (std::uint32_t thread = 0; thread < kThreadsPerLane; ++thread) {
        thread_input[thread] = static_cast<std::uint32_t>(
            (thread == 3) * lane_blocks + (thread == 4) + (thread == 5) * 2 + (thread == 6));
    }

    Warp addr{};
    BlockL tmp{};
    next_addresses1(addr, tmp, thread_input);

    auto step_indexed = [&](std::uint32_t slice, std::uint32_t offset) {
        const std::uint32_t addr_index = offset % kQwordsInBlock;
        if (addr_index == 0) {
            thread_input[6] += 1;
            next_addresses1(addr, tmp, thread_input);
        }
        const std::uint32_t raw = extract_ref_index(addr, addr_index);
        table.push_back(computeRefPos(segment_blocks, slice, offset, raw));
    };

    for (std::uint32_t offset = 2; offset < segment_blocks; ++offset) {
        step_indexed(0, offset);
    }

    thread_input[2] += 1;
    thread_input[6] = 0;

    for (std::uint32_t offset = 0; offset < segment_blocks; ++offset) {
        step_indexed(1, offset);
    }

    return table;
}

} // namespace hashapi
