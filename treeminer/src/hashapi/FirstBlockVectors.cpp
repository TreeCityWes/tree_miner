#include "FirstBlockVectors.h"

#include "../argon2-common.h"
#include "../argon2params.h"

#include <cstring>

namespace hashapi {
namespace {

char hexNibble(std::uint8_t value)
{
    return "0123456789abcdef"[value & 0xf];
}

int hexValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

} // namespace

FirstBlockPair fillHostFirstBlocks(const std::string& salt_hex,
                                   const std::string& key,
                                   std::uint32_t difficulty)
{
    Argon2Params params(argon2::ARGON2_ID, argon2::ARGON2_VERSION_13,
                        64, salt_hex, nullptr, 0, nullptr, 0,
                        1, difficulty, 1);
    std::uint8_t memory[2 * kFirstBlockBytes];
    params.fillFirstBlocks(memory, key.data(), key.size());
    FirstBlockPair pair;
    std::memcpy(pair.block0.data(), memory, kFirstBlockBytes);
    std::memcpy(pair.block1.data(), memory + kFirstBlockBytes, kFirstBlockBytes);
    return pair;
}

std::string bytesToHex(const std::uint8_t* data, std::size_t len)
{
    std::string hex;
    hex.resize(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        hex[2 * i] = hexNibble(static_cast<std::uint8_t>(data[i] >> 4));
        hex[2 * i + 1] = hexNibble(data[i]);
    }
    return hex;
}

bool parseHexBytes(const std::string& hex, std::uint8_t* out, std::size_t len)
{
    if (hex.size() != len * 2) {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i) {
        const int hi = hexValue(hex[2 * i]);
        const int lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace hashapi
