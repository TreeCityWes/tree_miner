#include "DeviceFinalize.h"

#include "HashApiEncoding.h"
#include "../argon2-common.h"
#include "../argon2params.h"

#include <cstring>

namespace hashapi {

HostHitBuffer::HostHitBuffer(std::size_t capacity)
    : hits_(capacity)
{
}

bool HostHitBuffer::tryPush(DeviceFinalizeHit hit)
{
    if (size_ >= hits_.size()) {
        ++dropped_;
        return false;
    }
    hits_[size_] = hit;
    ++size_;
    return true;
}

void HostHitBuffer::clear()
{
    size_ = 0;
    dropped_ = 0;
}

FinalizeScan scanFinalizedHash(const std::string& hash,
                               const std::string& xen_pattern,
                               bool allow_xuni)
{
    FinalizeScan scan;
    if (!xen_pattern.empty() && hash.find(xen_pattern) != std::string::npos) {
        scan.xen11 = true;
    }
    if (allow_xuni && hasXuniMatch(hash)) {
        scan.xuni = true;
    }
    return scan;
}

void cpuFinalizeLastBlock(const void* last_block,
                          std::uint8_t digest[kDeviceFinalizeDigestBytes])
{
    Argon2Params params(argon2::ARGON2_ID, argon2::ARGON2_VERSION_13,
                        kDeviceFinalizeDigestBytes, std::string(),
                        nullptr, 0, nullptr, 0, 1, 8, 1);
    params.finalize(digest, last_block);
}

} // namespace hashapi
