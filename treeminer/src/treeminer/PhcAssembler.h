#pragma once
// Assembles the complete PHC-encoded Argon2id string for a find from the parameters the GPU
// batch actually used — no CPU Argon2 recompute, no dependence on mutable global difficulty.
// This is the fix for the upstream stale-difficulty silent drop (see CHANGES-FROM-UPSTREAM.md).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "../hashapi/HashApiEncoding.h"

namespace treeminer {

inline std::vector<std::uint8_t> hexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument("hexToBytes: odd-length hex string");
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("hexToBytes: non-hex character");
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return bytes;
}

// digest_b64 is the unpadded-base64 digest as produced on-GPU (hashapi::base64Encode format,
// PHC-compatible). hexsalt is the 40-hex-char ETH address without 0x prefix.
inline std::string assemblePhc(std::uint32_t memory_cost, const std::string& hexsalt,
                               const std::string& digest_b64) {
    const auto salt_bytes = hexToBytes(hexsalt);
    const std::string salt_b64 = hashapi::base64Encode(salt_bytes.data(), salt_bytes.size());
    std::string phc;
    phc.reserve(64 + salt_b64.size() + digest_b64.size());
    phc.append("$argon2id$v=19$m=");
    phc.append(std::to_string(memory_cost));
    phc.append(",t=1,p=1$");
    phc.append(salt_b64);
    phc.push_back('$');
    phc.append(digest_b64);
    return phc;
}

}  // namespace treeminer
