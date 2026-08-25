#pragma once

#include "HashApiMatching.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hashapi {

// Host ownership model for device-side finalize (PLAN §10.10).
// Upstream died because the output buffer was reused or freed before DtoH
// completed. The hit buffer is allocated by the host, lives across the kernel
// launch + stream sync, and is the only device output copied back: hits, not
// batch × 1 KiB last blocks.
//
// Device kernel (not in this PR) would atomicAdd into count and write
// hits[slot]. Host copies min(count, capacity) after synchronize. Overflow
// increments dropped and those jobs fall back to the CPU finalize path.

constexpr std::size_t kDeviceFinalizeDigestBytes = 64;

enum class FinalizeHitKind : std::uint8_t {
    None = 0,
    Xen11 = 1,
    Xuni = 2,
    Both = 3,
};

struct DeviceFinalizeHit {
    std::uint32_t job_index = 0;
    std::uint8_t digest[kDeviceFinalizeDigestBytes]{};
    FinalizeHitKind kind = FinalizeHitKind::None;
};

class HostHitBuffer {
public:
    explicit HostHitBuffer(std::size_t capacity);

    std::size_t capacity() const { return hits_.size(); }
    std::size_t size() const { return size_; }
    std::size_t dropped() const { return dropped_; }
    const DeviceFinalizeHit* data() const { return hits_.data(); }

    // Host-side push used by CPU goldens and as the overflow fallback.
    bool tryPush(DeviceFinalizeHit hit);
    void clear();

private:
    std::vector<DeviceFinalizeHit> hits_;
    std::size_t size_ = 0;
    std::size_t dropped_ = 0;
};

struct FinalizeScan {
    bool xen11 = false;
    bool xuni = false;
};

FinalizeScan scanFinalizedHash(const std::string& hash,
                               const std::string& xen_pattern = "XEN11",
                               bool allow_xuni = true);

inline FinalizeHitKind hitKindFromScan(FinalizeScan scan)
{
    if (scan.xen11 && scan.xuni) {
        return FinalizeHitKind::Both;
    }
    if (scan.xen11) {
        return FinalizeHitKind::Xen11;
    }
    if (scan.xuni) {
        return FinalizeHitKind::Xuni;
    }
    return FinalizeHitKind::None;
}

// Blake2b-long digest of a 1 KiB last block → 64-byte Argon2 tag (lanes=1).
void cpuFinalizeLastBlock(const void* last_block,
                          std::uint8_t digest[kDeviceFinalizeDigestBytes]);

} // namespace hashapi
