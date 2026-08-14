// GpuMemoryPlanner: the per-device ledger that serializes multi-stream VRAM sizing.
// Pure C++ — the planner never touches CUDA; free memory arrives via a callback.

#include "GpuMemoryPlanner.h"

#include <cassert>
#include <cstdio>

namespace {
constexpr std::size_t MiB = 1024ULL * 1024ULL;
}

int main()
{
    auto& planner = GpuMemoryPlanner::instance();
    planner.declareStream(0, 0);
    planner.declareStream(0, 1);

    // Boot: both streams size before either allocates. Stream 1 must see stream 0's
    // reservation as PENDING (it still shows in free memory) and not overcommit.
    std::size_t free_b = 24064 * MiB;
    std::size_t seen_share0 = 0;
    const std::size_t pool0 = planner.planPool(0, 0, [&] { return free_b; },
                                               [&](std::size_t s) {
                                                   seen_share0 = s;
                                                   return s;  // take the full share
                                               });
    assert(seen_share0 == 11776 * MiB);
    assert(pool0 == 11776 * MiB);

    std::size_t seen_share1 = 0;
    planner.planPool(0, 1, [&] { return free_b; },
                     [&](std::size_t s) {
                         seen_share1 = s;
                         return s;
                     });
    assert(seen_share1 == 11776 * MiB);
    assert(seen_share0 + seen_share1 <= free_b);

    // Stream 0 allocates and commits; stream 1 resizing now counts it as COMMITTED
    // (part of the shared pool) rather than pending.
    planner.confirmPool(0, 0);
    free_b = 24064 * MiB - pool0;  // stream 0's pool left free memory
    planner.releasePool(0, 1);
    std::size_t seen_share1b = 0;
    planner.planPool(0, 1, [&] { return free_b; },
                     [&](std::size_t s) {
                         seen_share1b = s;
                         return s;
                     });
    assert(seen_share1b == 11776 * MiB);  // (free + committed - headroom) / 2

    // A reservation smaller than the share (e.g. a tuned batch cap) is what gets
    // recorded — siblings' shares stay honest.
    planner.releasePool(0, 0);
    planner.planPool(0, 0, [&] { return 24064 * MiB - seen_share1b; },
                     [&](std::size_t) { return 2048 * MiB; });

    // Retirement: a dead sibling stops counting toward the divisor and its memory is
    // reclaimed at the survivor's next resize.
    planner.retireStream(0, 0);
    planner.releasePool(0, 1);
    std::size_t seen_share_solo = 0;
    planner.planPool(0, 1, [&] { return 24064 * MiB; },
                     [&](std::size_t s) {
                         seen_share_solo = s;
                         return s;
                     });
    assert(seen_share_solo == 24064 * MiB - 512 * MiB);  // divisor is 1 again

    // Lazy registration: sizing an undeclared (device, stream) still works, solo.
    std::size_t seen_lazy = 0;
    planner.planPool(7, 0, [&] { return 4096 * MiB; },
                     [&](std::size_t s) {
                         seen_lazy = s;
                         return s;
                     });
    assert(seen_lazy == 3584 * MiB);

    std::puts("gpu_memory_planner: all assertions passed");
    return 0;
}
