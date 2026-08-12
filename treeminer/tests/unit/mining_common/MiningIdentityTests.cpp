#include "MiningCommon.h"

#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>

int main()
{
    const std::string addressA = "0x" + std::string(4096, 'a');
    const std::string addressB = "0x" + std::string(4096, 'b');
    const std::string prefixA(2048, 'c'), prefixB(2048, 'd');
    const std::string patternA(1024, 'e'), patternB(1024, 'f');

    setMiningUserAddress(addressA);
    setSelfMiningPrefix(prefixA);
    setTestBlockPattern(patternA);

    std::atomic<bool> stop{false};
    std::atomic<bool> valid{true};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto snapshot = miningIdentitySnapshot();
                if ((snapshot->userAddress != addressA && snapshot->userAddress != addressB) ||
                    (snapshot->selfMiningPrefix != prefixA && snapshot->selfMiningPrefix != prefixB) ||
                    (snapshot->testBlockPattern != patternA && snapshot->testBlockPattern != patternB)) {
                    valid.store(false, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int i = 0; i < 10000; ++i) {
        const bool alternate = (i % 2) != 0;
        setMiningUserAddress(alternate ? addressB : addressA);
        setSelfMiningPrefix(alternate ? prefixB : prefixA);
        setTestBlockPattern(alternate ? patternB : patternA);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();

    assert(valid.load());
    const auto finalSnapshot = miningIdentitySnapshot();
    assert(finalSnapshot->userAddress == addressB);
    assert(finalSnapshot->selfMiningPrefix == prefixB);
    assert(finalSnapshot->testBlockPattern == patternB);
}
