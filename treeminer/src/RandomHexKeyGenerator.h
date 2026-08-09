#pragma once

#include <iostream>
#include <string>
#include <random>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>

class RandomHexKeyGenerator {
public:
    RandomHexKeyGenerator(const std::string& initial_prefix = "", size_t key_length = 64)
        : total_length(key_length) {
            setPrefix(initial_prefix);
            // Fleet-scale collision safety: upstream seeded mt19937 from a single 32-bit value
            // (2^32 possible key streams — birthday collisions across a large fleet). Seed a
            // 64-bit engine from 256 bits of OS entropy plus a per-instance counter and clock.
            std::random_device rd;
            std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd(),
                              static_cast<unsigned int>(
                                  std::chrono::high_resolution_clock::now().time_since_epoch().count()),
                              instanceCounter()++};
            generator.seed(seq);
        }

    void setPrefix(const std::string& new_prefix) {
        prefix = new_prefix;
        std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                       [](unsigned char c){ return std::tolower(c); });
    }

    std::string nextRandomKey() {
        if (prefix.length() >= total_length) {
            std::cout << "Warning: Prefix is longer than total length. Returning prefix." << std::endl;
            return prefix.substr(0, total_length);
        }

        std::string key;
        key.reserve(total_length);
        key.append(prefix);
        while (key.length() < total_length) {
            std::uint64_t random_bits = generator();
            for (int nibble = 0; nibble < 16 && key.length() < total_length; ++nibble) {
                key.push_back(kHexChars[random_bits & 0x0f]);
                random_bits >>= 4;
            }
        }
        return key;
    }

private:
    static std::atomic<unsigned int>& instanceCounter() {
        static std::atomic<unsigned int> counter{0};
        return counter;
    }

    inline static constexpr char kHexChars[] = "0123456789abcdef";
    std::string prefix;
    size_t total_length;
    std::mt19937_64 generator;
};
