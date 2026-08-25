#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hashapi {

// Committed CPU first-block goldens (2 × 1 KiB) for a fixed (salt, key, m).
// A GPU box diffs device blocks 0/1 against these files; this environment
// only proves the host path is stable.

constexpr std::size_t kFirstBlockBytes = 1024;
constexpr std::uint32_t kFirstBlockGoldenDifficulty = 8;
constexpr char kFirstBlockGoldenSalt[] = "e4bb184781bbc9c7004e8dafd4a9b49d203bc9bc";
constexpr char kFirstBlockGoldenKey[] =
    "52a13632690c0d5a7e528c91c8462f9d68d24975d4f80cc64d20504063f3590f";

struct FirstBlockPair {
    std::array<std::uint8_t, kFirstBlockBytes> block0{};
    std::array<std::uint8_t, kFirstBlockBytes> block1{};
};

FirstBlockPair fillHostFirstBlocks(const std::string& salt_hex,
                                   const std::string& key,
                                   std::uint32_t difficulty);

std::string bytesToHex(const std::uint8_t* data, std::size_t len);
bool parseHexBytes(const std::string& hex, std::uint8_t* out, std::size_t len);

} // namespace hashapi
