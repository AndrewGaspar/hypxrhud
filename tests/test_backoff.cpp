#include "doctest.h"

#include "Backoff.hpp"

using namespace hud;

TEST_CASE("backoff: doubling schedule clamped to cap (compositor parity)") {
    // base=2000, cap=30000 -> 2s, 4s, 8s, 16s, 30s(cap), 30s...
    CHECK(reprobeBackoffMs(0, 2000, 30000) == 2000);
    CHECK(reprobeBackoffMs(1, 2000, 30000) == 4000);
    CHECK(reprobeBackoffMs(2, 2000, 30000) == 8000);
    CHECK(reprobeBackoffMs(3, 2000, 30000) == 16000);
    CHECK(reprobeBackoffMs(4, 2000, 30000) == 30000); // 32000 clamped
    CHECK(reprobeBackoffMs(5, 2000, 30000) == 30000);
    CHECK(reprobeBackoffMs(50, 2000, 30000) == 30000);
}

TEST_CASE("backoff: uses the shipped defaults") {
    CHECK(reprobeBackoffMs(0, kReprobeBaseMs, kReprobeCapMs) == 2000);
    CHECK(reprobeBackoffMs(4, kReprobeBaseMs, kReprobeCapMs) == kReprobeCapMs);
}

TEST_CASE("backoff: degenerate inputs are sanitised, never below base or above cap") {
    CHECK(reprobeBackoffMs(0, 0, 30000) == 2000);      // base<=0 -> 2000 default
    CHECK(reprobeBackoffMs(3, 5000, 1000) == 5000);    // cap<base -> cap raised to base
    CHECK(reprobeBackoffMs(-1, 2000, 30000) == 2000);  // negative attempt -> base
    // Monotonic non-decreasing.
    int64_t prev = 0;
    for (int a = 0; a < 20; ++a) {
        int64_t v = reprobeBackoffMs(a, 2000, 30000);
        CHECK(v >= prev);
        CHECK(v <= 30000);
        prev = v;
    }
}
