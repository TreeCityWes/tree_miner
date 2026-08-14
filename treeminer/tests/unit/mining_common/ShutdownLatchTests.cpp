// interruptibleShutdownSleep / notifyShutdownSleepers: the latch the long-lived poller
// threads sleep on so main() can join them promptly at shutdown instead of waiting out
// multi-minute sleep_for calls (the shutdown use-after-free fix depends on this).

#include "MiningCommon.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono;

int main()
{
    // A short sleep with no shutdown in flight elapses fully and reports "keep running".
    running.store(true);
    {
        const auto t0 = steady_clock::now();
        const bool kept_running = interruptibleShutdownSleep(milliseconds(50));
        const auto elapsed = steady_clock::now() - t0;
        assert(kept_running);
        assert(elapsed >= milliseconds(50));
    }

    // A sleeper parked on a long sleep wakes promptly once shutdown is requested.
    {
        std::atomic<bool> woke{false};
        std::atomic<bool> kept_running{true};
        std::thread sleeper([&] {
            kept_running.store(interruptibleShutdownSleep(minutes(10)));
            woke.store(true);
        });
        std::this_thread::sleep_for(milliseconds(50));
        assert(!woke.load());  // still parked on the 10-minute sleep
        const auto t0 = steady_clock::now();
        running.store(false);
        notifyShutdownSleepers();
        sleeper.join();
        const auto wake_latency = steady_clock::now() - t0;
        assert(woke.load());
        assert(!kept_running.load());
        assert(wake_latency < milliseconds(500));
    }

    // With shutdown already requested, the sleep returns immediately.
    {
        const auto t0 = steady_clock::now();
        assert(!interruptibleShutdownSleep(minutes(10)));
        assert(steady_clock::now() - t0 < milliseconds(500));
    }

    // A stray notify with no sleeper parked is harmless.
    notifyShutdownSleepers();

    std::puts("shutdown_latch: all assertions passed");
    return 0;
}
