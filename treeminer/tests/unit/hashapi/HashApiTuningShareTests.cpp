// computeStreamMemoryShare: the pure fair-share math behind multi-stream VRAM sizing.
// The scenarios mirror the field failure this replaces: the old total-memory clamp let
// the first stream take the whole free pool under external VRAM pressure, starving its
// sibling permanently.

#include "hashapi/HashApiTuning.h"

#include <cassert>
#include <cstdio>

namespace {

constexpr std::size_t MiB = 1024ULL * 1024ULL;
constexpr std::size_t kHeadroom = 512ULL * MiB;

std::size_t share(std::size_t free_b, std::size_t committed, std::size_t pending,
                  std::size_t streams) {
    hashapi::StreamShareInput in;
    in.device_free_bytes = free_b;
    in.sibling_committed_bytes = committed;
    in.sibling_pending_bytes = pending;
    in.active_streams = streams;
    in.headroom_bytes = kHeadroom;
    return hashapi::computeStreamMemoryShare(in);
}

} // namespace

int main()
{
    // Boot split: both streams size before either allocates; shares must not overcommit.
    {
        const std::size_t free_b = 24064 * MiB;  // 23.5 GiB
        const std::size_t s0 = share(free_b, 0, 0, 2);
        assert(s0 == 11776 * MiB);  // (23.5 GiB - 512 MiB) / 2
        const std::size_t s1 = share(free_b, 0, s0, 2);
        assert(s1 == 11776 * MiB);
        assert(s0 + s1 <= free_b);
    }

    // External-process pressure: the pool is what the miner can actually use, whether
    // the sibling's half is still pending or already committed.
    {
        assert(share(13824 * MiB, 0, 6656 * MiB, 2) == 6656 * MiB);  // sibling pending
        assert(share(7168 * MiB, 6656 * MiB, 0, 2) == 6656 * MiB);   // sibling committed
    }

    // Starved sibling (the reported bug): scraps of free memory next to a stale
    // oversized pool must still yield a nonzero share...
    {
        const std::size_t s = share(410 * MiB, 13824 * MiB, 0, 2);
        assert(s == 410 * MiB);
        assert(s > 0);
        // ...and the oversized holder's next resize is capped at the fair share, so it
        // can no longer re-grab the whole pool.
        const std::size_t resized = share(13824 * MiB, 410 * MiB, 0, 2);
        assert(resized == (14234 * MiB - kHeadroom) / 2);
    }

    // Resize convergence: alternating release->size->allocate rounds reach the fair
    // split within two rounds even from the fully-starved state.
    {
        const std::size_t avail = 14336 * MiB;  // memory available to the miner
        std::size_t pools[2] = {13824 * MiB, 0};
        const std::size_t fair_target = (avail - kHeadroom) / 2;
        for (int round = 0; round < 2; ++round) {
            for (int i : {1, 0}) {  // the starved stream resizes first — worst case
                const std::size_t other = pools[1 - i];
                pools[i] = share(avail - other, other, 0, 2);  // own pool released
            }
        }
        assert(pools[0] == fair_target);
        assert(pools[1] == fair_target);
    }

    // Degenerates.
    assert(share(256 * MiB, 0, 0, 2) == 0);           // pool cannot cover headroom
    assert(share(1024 * MiB, 0, 1024 * MiB, 2) == 0); // pending exhausts free: back off
    assert(share(8192 * MiB, 0, 0, 1) == 7680 * MiB); // single stream: free - headroom
    assert(share(8192 * MiB, 0, 0, 0) == 7680 * MiB); // zero streams treated as one

    std::puts("hashapi_tuning_share: all assertions passed");
    return 0;
}
