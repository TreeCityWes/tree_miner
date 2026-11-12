// RollingRate: recent throughput from a cumulative counter (src/treeminer/RollingRate.h).
#include "../../../src/treeminer/RollingRate.h"
#include "test_framework.h"

using treeminer::RollingRate;

int main() {
    TEST_CASE("no rate until the minimum span of history exists");
    {
        RollingRate r;
        CHECK(!r.rate());
        r.add(0, 0);
        CHECK(!r.rate());
        r.add(4000, 4000);
        CHECK(!r.rate());  // 4 s < 5 s minimum
        r.add(5000, 5000);
        CHECK(r.rate().has_value());
        CHECK(r.rate().value_or(0) > 999.0 && r.rate().value_or(0) < 1001.0);
    }

    TEST_CASE("a steady stream reports its rate, per-batch calls stay bounded");
    {
        RollingRate r;
        std::uint64_t total = 0;
        for (std::int64_t t = 0; t <= 600000; t += 57) {  // one 448-hash batch per 57 ms
            total += 448;
            r.add(t, total);
        }
        const double rate = r.rate().value_or(0);
        CHECK(rate > 7800.0 && rate < 7900.0);  // 448 / 0.057 s = 7860 H/s
    }

    TEST_CASE("a drop shows within the window, unlike a since-start average");
    {
        RollingRate r;
        std::uint64_t total = 0;
        std::int64_t t = 0;
        for (; t <= 6000000; t += 1000) {  // 100 minutes at 8000 H/s
            total += 8000;
            r.add(t, total);
        }
        for (std::int64_t end = t + 60000; t <= end; t += 1000) {  // then 60 s at half speed
            total += 4000;
            r.add(t, total);
        }
        const double recent = r.rate().value_or(0);
        const double lifetime = static_cast<double>(total) * 1000.0 / static_cast<double>(t);
        CHECK(recent > 3900.0 && recent < 4200.0);
        CHECK(lifetime > 7900.0);  // the average has barely moved
    }

    TEST_CASE("a stalled stream is detectable by sample age");
    {
        RollingRate r;
        r.add(0, 0);
        r.add(10000, 80000);
        CHECK(r.ageMs(10000).value_or(-1) == 0);
        CHECK(r.ageMs(190000).value_or(-1) == 180000);
    }

    TEST_CASE("a counter reset starts a fresh window instead of a negative rate");
    {
        RollingRate r;
        r.add(0, 1000000);
        r.add(10000, 1080000);
        CHECK(r.rate().value_or(0) > 7999.0);
        r.add(11000, 500);  // stream restarted
        CHECK(!r.rate());
        r.add(17000, 48500);
        CHECK(r.rate().value_or(0) > 7999.0 && r.rate().value_or(0) < 8001.0);
    }

    return testfw::summary("rolling_rate");
}
