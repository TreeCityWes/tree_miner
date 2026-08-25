#include "hashapi/DeviceFinalize.h"
#include "hashapi/HashApiEncoding.h"

#include "argon2-common.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    try {
        hashapi::HostHitBuffer buffer(2);
        require(buffer.capacity() == 2, "capacity");
        require(buffer.size() == 0, "empty");

        hashapi::DeviceFinalizeHit hit{};
        hit.job_index = 7;
        hit.kind = hashapi::FinalizeHitKind::Xen11;
        require(buffer.tryPush(hit), "first push");
        hit.job_index = 9;
        require(buffer.tryPush(hit), "second push");
        require(!buffer.tryPush(hit), "overflow rejected");
        require(buffer.dropped() == 1, "overflow counted");
        require(buffer.size() == 2, "full");
        require(buffer.data()[0].job_index == 7, "first hit preserved");
        require(buffer.data()[1].job_index == 9, "second hit preserved");

        buffer.clear();
        require(buffer.size() == 0, "cleared");
        require(buffer.dropped() == 0, "drop counter reset");
        require(buffer.tryPush(hit), "reusable after clear");

        const auto xen = hashapi::scanFinalizedHash("aaaXEN11bbb", "XEN11", true);
        require(xen.xen11 && !xen.xuni, "XEN11 only");
        const auto xuni = hashapi::scanFinalizedHash("fooXUNI7bar", "XEN11", true);
        require(!xuni.xen11 && xuni.xuni, "XUNI only");
        const auto both = hashapi::scanFinalizedHash("XEN11-and-XUNI0", "XEN11", true);
        require(both.xen11 && both.xuni, "both");
        require(hashapi::hitKindFromScan(both) == hashapi::FinalizeHitKind::Both, "both kind");
        const auto muted = hashapi::scanFinalizedHash("XUNI0", "XEN11", false);
        require(!muted.xuni, "xuni disabled");

        std::array<std::uint8_t, argon2::ARGON2_BLOCK_SIZE> last{};
        last[0] = 0x11;
        last[1] = 0x22;
        last[1023] = 0xab;
        std::uint8_t digest_a[hashapi::kDeviceFinalizeDigestBytes];
        std::uint8_t digest_b[hashapi::kDeviceFinalizeDigestBytes];
        hashapi::cpuFinalizeLastBlock(last.data(), digest_a);
        hashapi::cpuFinalizeLastBlock(last.data(), digest_b);
        require(std::memcmp(digest_a, digest_b, sizeof(digest_a)) == 0, "finalize deterministic");

        std::array<std::uint8_t, argon2::ARGON2_BLOCK_SIZE> other = last;
        other[10] = 0xff;
        std::uint8_t digest_c[hashapi::kDeviceFinalizeDigestBytes];
        hashapi::cpuFinalizeLastBlock(other.data(), digest_c);
        require(std::memcmp(digest_a, digest_c, sizeof(digest_a)) != 0, "input affects digest");

        const std::string b64 = hashapi::base64Encode(digest_a, sizeof(digest_a));
        require(!b64.empty(), "base64 of digest");
        require(b64.find('=') == std::string::npos, "hash-api base64 is unpadded");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
