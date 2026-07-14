#include "doctest.h"

#include "battery/BatteryConfig.hpp"
#include "battery/BatteryModel.hpp"

using namespace hudbat;

namespace {
    SSourceReading src(bool present, float pct, bool charging) {
        SSourceReading r;
        r.present = present;
        r.percent = pct;
        r.charging = charging;
        return r;
    }
}

TEST_CASE("buildGauges: omits absent/unknown sources and orders headset before laptop") {
    SModelParams p; // both shown, default labels.

    SUBCASE("both present -> headset first, laptop second") {
        auto g = buildGauges(src(true, 55, false), src(true, 83, true), p);
        REQUIRE(g.size() == 2);
        CHECK(g[0].label == "headset");
        CHECK(g[0].percent == doctest::Approx(55));
        CHECK(g[0].charging == false);
        CHECK(g[1].label == "laptop");
        CHECK(g[1].percent == doctest::Approx(83));
        CHECK(g[1].charging == true);
    }
    SUBCASE("headset absent -> laptop-only, no stale headset gauge") {
        auto g = buildGauges(src(false, -1, false), src(true, 83, false), p);
        REQUIRE(g.size() == 1);
        CHECK(g[0].label == "laptop");
    }
    SUBCASE("present but unknown percent is omitted (never a fabricated value)") {
        auto g = buildGauges(src(true, -1, false), src(true, 83, false), p);
        REQUIRE(g.size() == 1);
        CHECK(g[0].label == "laptop");
    }
    SUBCASE("show flags suppress a gauge even when present") {
        p.showLaptop = false;
        auto g = buildGauges(src(true, 55, false), src(true, 83, false), p);
        REQUIRE(g.size() == 1);
        CHECK(g[0].label == "headset");
    }
    SUBCASE("no sources -> empty (panel dismissed by the client)") {
        auto g = buildGauges(src(false, -1, false), src(false, -1, false), p);
        CHECK(g.empty());
    }
    SUBCASE("percent clamps to [0,100]") {
        auto g = buildGauges(src(true, 150, false), src(false, -1, false), p);
        REQUIRE(g.size() == 1);
        CHECK(g[0].percent == doctest::Approx(100));
    }
    SUBCASE("custom labels are honoured") {
        p.headsetLabel = "quest";
        p.laptopLabel = "thinkpad";
        auto g = buildGauges(src(true, 10, false), src(true, 20, false), p);
        REQUIRE(g.size() == 2);
        CHECK(g[0].label == "quest");
        CHECK(g[1].label == "thinkpad");
    }
}

TEST_CASE("gaugesEqual: the zero-cost-when-static diff") {
    SModelParams p;
    auto a = buildGauges(src(true, 55.2f, false), src(true, 83.0f, true), p);

    SUBCASE("sub-1% jitter is equal (no re-send)") {
        auto b = buildGauges(src(true, 55.4f, false), src(true, 83.4f, true), p);
        CHECK(gaugesEqual(a, b));
    }
    SUBCASE("a 1% change is not equal") {
        auto b = buildGauges(src(true, 56.7f, false), src(true, 83.0f, true), p);
        CHECK_FALSE(gaugesEqual(a, b));
    }
    SUBCASE("a charging-flag flip is not equal") {
        auto b = buildGauges(src(true, 55.2f, true), src(true, 83.0f, true), p);
        CHECK_FALSE(gaugesEqual(a, b));
    }
    SUBCASE("a source appearing/disappearing is not equal") {
        auto b = buildGauges(src(false, -1, false), src(true, 83.0f, true), p);
        CHECK_FALSE(gaugesEqual(a, b));
    }
    SUBCASE("identical lists are equal") {
        CHECK(gaugesEqual(a, a));
    }
}

TEST_CASE("lowBatteryLatch: one-shot per discharge cycle") {
    const int thr = 15, hys = 5;

    SUBCASE("fires once when discharging at/below threshold, then stays quiet") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(true, 14, false), thr, hys) == true);  // fire
        CHECK(lowBatteryLatch(l, src(true, 13, false), thr, hys) == false); // still low, no re-fire
        CHECK(lowBatteryLatch(l, src(true, 10, false), thr, hys) == false);
    }
    SUBCASE("exactly at threshold fires") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(true, 15, false), thr, hys) == true);
    }
    SUBCASE("above threshold never fires") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(true, 16, false), thr, hys) == false);
    }
    SUBCASE("charging re-arms; a later dip fires again") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(true, 14, false), thr, hys) == true);   // fire
        CHECK(lowBatteryLatch(l, src(true, 14, true), thr, hys) == false);   // plugged in -> re-arm
        CHECK(lowBatteryLatch(l, src(true, 12, false), thr, hys) == true);   // unplug, dip -> fire
    }
    SUBCASE("rising past threshold+hysteresis re-arms") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(true, 14, false), thr, hys) == true);   // fire
        CHECK(lowBatteryLatch(l, src(true, 20, false), thr, hys) == false);  // 15+5 -> re-arm
        CHECK(lowBatteryLatch(l, src(true, 14, false), thr, hys) == true);   // dip -> fire again
    }
    SUBCASE("just below the re-arm line does NOT re-arm") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(true, 14, false), thr, hys) == true);
        CHECK(lowBatteryLatch(l, src(true, 19, false), thr, hys) == false);  // <20, still disarmed
        CHECK(lowBatteryLatch(l, src(true, 14, false), thr, hys) == false);  // no re-fire
    }
    SUBCASE("absent/unknown source leaves the latch untouched") {
        SLatch l;
        CHECK(lowBatteryLatch(l, src(false, -1, false), thr, hys) == false);
        CHECK(l.armed == true);
        // armed carries through: a real low reading then fires.
        CHECK(lowBatteryLatch(l, src(true, 10, false), thr, hys) == true);
    }
}

TEST_CASE("source value parsing") {
    SUBCASE("upowerCharging maps the UPower State enum") {
        CHECK(upowerCharging(1) == true);   // Charging
        CHECK(upowerCharging(2) == false);  // Discharging
        CHECK(upowerCharging(3) == false);  // Empty
        CHECK(upowerCharging(4) == true);   // FullyCharged
        CHECK(upowerCharging(5) == true);   // PendingCharge
        CHECK(upowerCharging(6) == false);  // PendingDischarge
    }
    SUBCASE("upowerIsLaptopBattery requires Battery type AND present") {
        CHECK(upowerIsLaptopBattery(2, true) == true);
        CHECK(upowerIsLaptopBattery(2, false) == false); // not present
        CHECK(upowerIsLaptopBattery(0, true) == false);  // Unknown (desktop DisplayDevice)
        CHECK(upowerIsLaptopBattery(1, true) == false);  // LinePower
    }
    SUBCASE("wivrnChargeToPercent scales [0,1] -> [0,100] and clamps") {
        CHECK(wivrnChargeToPercent(0.0) == doctest::Approx(0));
        CHECK(wivrnChargeToPercent(0.42) == doctest::Approx(42));
        CHECK(wivrnChargeToPercent(1.0) == doctest::Approx(100));
        CHECK(wivrnChargeToPercent(1.5) == doctest::Approx(100)); // clamp
        CHECK(wivrnChargeToPercent(-0.2) == doctest::Approx(0));  // clamp
    }
    SUBCASE("lowBatteryToastText formats label + rounded percent") {
        CHECK(lowBatteryToastText("headset", 14.4f) == "headset battery 14%");
        CHECK(lowBatteryToastText("laptop", 4.6f) == "laptop battery 5%");
    }
}

TEST_CASE("parseBatteryConfig: TOML subset") {
    SUBCASE("all keys parse") {
        SBatteryConfig c;
        std::vector<std::string> e, w;
        const char* toml =
            "[battery]\n"
            "poll_interval_sec = 60\n"
            "low_threshold = 20\n"
            "low_hysteresis = 8\n"
            "headset = false\n"
            "laptop = true\n"
            "headset_label = \"quest 3\"\n"
            "laptop_label = \"laptop\"\n"
            "slot = \"battery\"\n"
            "toasts = false\n";
        CHECK(parseBatteryConfig(toml, c, e, w));
        CHECK(e.empty());
        CHECK(c.pollIntervalSec == 60);
        CHECK(c.lowThreshold == 20);
        CHECK(c.lowHysteresis == 8);
        CHECK(c.showHeadset == false);
        CHECK(c.showLaptop == true);
        CHECK(c.headsetLabel == "quest 3");
        CHECK(c.slot == "battery");
        CHECK(c.toasts == false);
    }
    SUBCASE("defaults hold when file omits keys") {
        SBatteryConfig c;
        std::vector<std::string> e, w;
        CHECK(parseBatteryConfig("[battery]\nlow_threshold = 10\n", c, e, w));
        CHECK(c.lowThreshold == 10);
        CHECK(c.pollIntervalSec == 30); // default
        CHECK(c.showHeadset == true);   // default
    }
    SUBCASE("unknown key warns, does not fail") {
        SBatteryConfig c;
        std::vector<std::string> e, w;
        CHECK(parseBatteryConfig("[battery]\nnope = 1\n", c, e, w));
        CHECK(e.empty());
        CHECK(w.size() == 1);
    }
    SUBCASE("bad type is a hard error") {
        SBatteryConfig c;
        std::vector<std::string> e, w;
        CHECK_FALSE(parseBatteryConfig("[battery]\npoll_interval_sec = notanumber\n", c, e, w));
        CHECK_FALSE(e.empty());
    }
    SUBCASE("bad bool is a hard error") {
        SBatteryConfig c;
        std::vector<std::string> e, w;
        CHECK_FALSE(parseBatteryConfig("[battery]\nheadset = yes\n", c, e, w));
        CHECK_FALSE(e.empty());
    }
}
