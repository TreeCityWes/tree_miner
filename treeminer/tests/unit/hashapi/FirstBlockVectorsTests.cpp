#include "hashapi/FirstBlockVectors.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef TREEMINER_GOLDEN_DIR
#define TREEMINER_GOLDEN_DIR "."
#endif

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readAll(const std::string& path)
{
    std::ifstream in(path);
    require(in.good(), "golden file missing");
    std::string hex;
    in >> hex;
    return hex;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const auto pair = hashapi::fillHostFirstBlocks(
            hashapi::kFirstBlockGoldenSalt,
            hashapi::kFirstBlockGoldenKey,
            hashapi::kFirstBlockGoldenDifficulty);
        const auto again = hashapi::fillHostFirstBlocks(
            hashapi::kFirstBlockGoldenSalt,
            hashapi::kFirstBlockGoldenKey,
            hashapi::kFirstBlockGoldenDifficulty);
        require(pair.block0 == again.block0 && pair.block1 == again.block1,
                "host first-blocks are deterministic");

        std::string other_key = hashapi::kFirstBlockGoldenKey;
        other_key.back() = other_key.back() == 'f' ? '0' : 'f';
        const auto other = hashapi::fillHostFirstBlocks(
            hashapi::kFirstBlockGoldenSalt, other_key, hashapi::kFirstBlockGoldenDifficulty);
        require(pair.block0 != other.block0, "different keys produce different block 0");
        require(pair.block1 != other.block1, "different keys produce different block 1");

        const std::string hex0 = hashapi::bytesToHex(pair.block0.data(), pair.block0.size());
        const std::string hex1 = hashapi::bytesToHex(pair.block1.data(), pair.block1.size());
        require(hex0.size() == 2048, "block0 hex is 1 KiB");
        require(hex1.size() == 2048, "block1 hex is 1 KiB");
        require(hex0 != hex1, "block 0 and 1 differ");

        if (argc >= 2 && std::string(argv[1]) == "--dump") {
            std::cout << hex0 << '\n' << hex1 << '\n';
            return 0;
        }

        const std::string dir = TREEMINER_GOLDEN_DIR;
        std::array<std::uint8_t, hashapi::kFirstBlockBytes> expected0{};
        std::array<std::uint8_t, hashapi::kFirstBlockBytes> expected1{};
        require(hashapi::parseHexBytes(readAll(dir + "/first-blocks-m8-selftest.block0.hex"),
                                       expected0.data(), expected0.size()),
                "block0 golden hex");
        require(hashapi::parseHexBytes(readAll(dir + "/first-blocks-m8-selftest.block1.hex"),
                                       expected1.data(), expected1.size()),
                "block1 golden hex");
        require(pair.block0 == expected0, "block 0 matches committed golden");
        require(pair.block1 == expected1, "block 1 matches committed golden");
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    return 0;
}
