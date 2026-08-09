// DrainScheduler unit tests — pure ordering + adaptive pacing.

#include <string>
#include <vector>

#include "submit/DrainScheduler.h"
#include "test_framework.h"

using treeminer::DifficultyTrend;
using treeminer::DrainScheduler;
using treeminer::FindKind;
using treeminer::FindRecord;
using treeminer::XuniWindowState;

namespace {

FindRecord rec(std::int64_t id, FindKind kind, std::uint32_t m) {
    FindRecord r;
    r.id = id;
    r.payload.key = "k" + std::to_string(id);
    r.payload.kind = kind;
    r.payload.memory_cost = m;
    return r;
}

XuniWindowState window(bool open, std::int64_t ms_until_close = 0) {
    XuniWindowState w;
    w.open = open;
    w.ms_until_close = ms_until_close;
    return w;
}

}  // namespace

int main() {
    DrainScheduler sched;

    // --- ordering ---
    TEST_CASE("empty backlog selects nothing");
    {
        std::vector<FindRecord> v;
        CHECK(sched.selectNext(v, DifficultyTrend::Unknown, window(false)) == nullptr);
    }

    TEST_CASE("oldest XEN11 first by default");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XEN11, 100000),
                                     rec(2, FindKind::XEN11, 90000),
                                     rec(3, FindKind::XEN11, 95000)};
        const FindRecord* p = sched.selectNext(v, DifficultyTrend::Unknown, window(false));
        CHECK(p != nullptr);
        CHECK_EQ(p ? p->id : -1, 1);  // journal order (oldest) wins when trend not rising
        p = sched.selectNext(v, DifficultyTrend::Falling, window(false));
        CHECK_EQ(p ? p->id : -1, 1);
        p = sched.selectNext(v, DifficultyTrend::Flat, window(false));
        CHECK_EQ(p ? p->id : -1, 1);
    }

    TEST_CASE("rising difficulty drains ascending-m first");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XEN11, 100000),
                                     rec(2, FindKind::XEN11, 90000),
                                     rec(3, FindKind::XEN11, 95000)};
        const FindRecord* p = sched.selectNext(v, DifficultyTrend::Rising, window(false));
        CHECK_EQ(p ? p->id : -1, 2);  // lowest m: closest to the rising floor
    }

    TEST_CASE("rising difficulty ties break oldest-first");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XEN11, 90000),
                                     rec(2, FindKind::XEN11, 90000)};
        const FindRecord* p = sched.selectNext(v, DifficultyTrend::Rising, window(false));
        CHECK_EQ(p ? p->id : -1, 1);
    }

    TEST_CASE("XUNI never selected while the window is closed");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XUNI, 100000)};
        CHECK(sched.selectNext(v, DifficultyTrend::Unknown, window(false)) == nullptr);
        // ...but a XEN11 in the same backlog still drains.
        v.push_back(rec(2, FindKind::XEN11, 100000));
        const FindRecord* p = sched.selectNext(v, DifficultyTrend::Unknown, window(false));
        CHECK_EQ(p ? p->id : -1, 2);
    }

    TEST_CASE("XUNI preempts XEN11 near the window end");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XEN11, 100000),
                                     rec(2, FindKind::XUNI, 100000),
                                     rec(3, FindKind::XUNI, 100000)};
        // 60s left: inside the default 120s preemption threshold.
        const FindRecord* p = sched.selectNext(v, DifficultyTrend::Unknown, window(true, 60000));
        CHECK_EQ(p ? p->id : -1, 2);  // oldest XUNI, ahead of the XEN11 backlog
    }

    TEST_CASE("XUNI yields to XEN11 while the window end is far");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XEN11, 100000),
                                     rec(2, FindKind::XUNI, 100000)};
        // 8 minutes of window left: XEN11 backlog first (SOL §7 ordering).
        const FindRecord* p = sched.selectNext(v, DifficultyTrend::Unknown, window(true, 480000));
        CHECK_EQ(p ? p->id : -1, 1);
        // With no XEN11 left, the XUNI drains even far from the end.
        std::vector<FindRecord> only_xuni = {rec(2, FindKind::XUNI, 100000)};
        p = sched.selectNext(only_xuni, DifficultyTrend::Unknown, window(true, 480000));
        CHECK_EQ(p ? p->id : -1, 2);
    }

    TEST_CASE("selectNext is const and repeatable");
    {
        std::vector<FindRecord> v = {rec(1, FindKind::XEN11, 100000),
                                     rec(2, FindKind::XEN11, 90000)};
        const FindRecord* a = sched.selectNext(v, DifficultyTrend::Rising, window(false));
        const FindRecord* b = sched.selectNext(v, DifficultyTrend::Rising, window(false));
        CHECK(a == b);
    }

    // --- adaptive pacing ---
    TEST_CASE("rate starts at 1/s and doubles per healthy round-trip up to the ceiling");
    {
        DrainScheduler s;  // defaults: start 1, max 4
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 1);
        CHECK_EQ(s.submitIntervalMs(), 1000);
        s.onHealthyRoundTrip();
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 2);
        s.onHealthyRoundTrip();
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 4);
        s.onHealthyRoundTrip();
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 4);  // ceiling: drain_rate config
        CHECK_EQ(s.submitIntervalMs(), 250);
    }

    TEST_CASE("5xx/429 halves the rate down to the floor");
    {
        DrainScheduler s;
        s.onHealthyRoundTrip();
        s.onHealthyRoundTrip();  // 4/s
        s.onThrottle();
        CHECK_EQ(static_cast<int>(s.ratePerSecond() * 100), 200);  // 2/s
        s.onThrottle();
        s.onThrottle();
        s.onThrottle();
        CHECK_EQ(static_cast<int>(s.ratePerSecond() * 100), 25);  // floor 0.25/s
        CHECK_EQ(s.submitIntervalMs(), 4000);
    }

    TEST_CASE("breaker close resets to the start rate");
    {
        DrainScheduler s;
        s.onHealthyRoundTrip();
        s.onHealthyRoundTrip();
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 4);
        s.onBreakerClose();
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 1);
    }

    TEST_CASE("ceiling is configurable");
    {
        DrainScheduler::Config cfg;
        cfg.max_rate_per_s = 8.0;
        DrainScheduler s(cfg);
        for (int i = 0; i < 6; ++i) {
            s.onHealthyRoundTrip();
        }
        CHECK_EQ(static_cast<int>(s.ratePerSecond()), 8);
    }

    return testfw::summary("drain_scheduler");
}
