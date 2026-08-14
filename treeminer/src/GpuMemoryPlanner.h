#pragma once
// Per-device VRAM sizing coordinator for multi-stream mining.
//
// Each CUDA stream of a device owns its own backend and sizes its batch pool
// independently; cudaMemGetInfo is device-wide, so unsynchronized sizing let the first
// stream take the entire free pool whenever another process held VRAM (the sibling then
// computed a zero batch and spun on "GPU memory allocation unavailable" forever). The
// planner serializes sizing per process and keeps a reserved-bytes ledger per (device,
// stream) so every stream can compute its fair share of the memory that is actually
// available to the miner — see hashapi::computeStreamMemoryShare for the math.
//
// Lifecycle per stream: declareStream (main, before mining threads start, so the divisor
// is right even if a sibling has not sized yet) -> [releasePool -> planPool -> ...mine...
// -> confirmPool] per rebuild -> retireStream on thread exit (a dead sibling's memory is
// reclaimed at the survivor's next resize). Single-stream mining never touches this.

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>

inline constexpr std::size_t kGpuDeviceHeadroomBytes = 512ULL * 1024ULL * 1024ULL;

class GpuMemoryPlanner {
public:
    static GpuMemoryPlanner& instance();

    void declareStream(int device, int stream);
    void retireStream(int device, int stream);

    // Serializes {query free memory -> choose pool size -> record it as a PENDING
    // reservation} under the planner lock. choosePoolBytes receives this stream's fair
    // share and returns the pool bytes it actually decided to use (which may be smaller,
    // e.g. a tuned batch cap — reserving the real pool keeps siblings' shares honest).
    std::size_t planPool(int device, int stream,
                         const std::function<std::size_t()>& queryFreeBytes,
                         const std::function<std::size_t(std::size_t share)>& choosePoolBytes);

    void confirmPool(int device, int stream);  // pending -> committed (pool allocated)
    void releasePool(int device, int stream);  // pool freed (call right after releaseBuffers)

private:
    struct Slot {
        std::size_t reserved = 0;
        bool committed = false;
    };

    GpuMemoryPlanner() = default;

    std::mutex mutex_;  // sizing is rare (boot + difficulty changes); one lock is plenty
    std::map<int, std::map<int, Slot>> devices_;
};
