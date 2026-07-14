#include "BatteryModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hudbat {

namespace {
    float clampPct(float p) {
        if (p < 0.f) return p;      // keep the "unknown" sentinel.
        if (p > 100.f) return 100.f;
        return p;
    }
    int roundedPct(float p) { return p < 0.f ? -1 : (int)std::lround(p); }
}

std::vector<hud::SGauge> buildGauges(const SSourceReading& headset,
                                     const SSourceReading& laptop,
                                     const SModelParams&   p) {
    std::vector<hud::SGauge> out;
    auto add = [&](const SSourceReading& r, bool show, const std::string& label) {
        if (!show || !r.present || r.percent < 0.f)
            return; // omit absent / unknown sources — never a stale gauge.
        out.push_back(hud::SGauge{label, clampPct(r.percent), r.charging});
    };
    add(headset, p.showHeadset, p.headsetLabel);
    add(laptop,  p.showLaptop,  p.laptopLabel);
    return out;
}

bool gaugesEqual(const std::vector<hud::SGauge>& a, const std::vector<hud::SGauge>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].label != b[i].label)
            return false;
        if (a[i].charging != b[i].charging)
            return false;
        if (roundedPct(a[i].percent) != roundedPct(b[i].percent))
            return false;
    }
    return true;
}

bool lowBatteryLatch(SLatch& latch, const SSourceReading& r, int lowThreshold, int lowHysteresis) {
    // A source we cannot read leaves the latch as-is (no fire, no re-arm).
    if (!r.present || r.percent < 0.f)
        return false;

    // Re-arm once the danger is clearly over: on AC, or comfortably back above the line.
    if (r.charging || r.percent >= (float)(lowThreshold + lowHysteresis)) {
        latch.armed = true;
        return false;
    }

    // Fire once per discharge cycle at/below the threshold while on battery.
    if (latch.armed && !r.charging && r.percent <= (float)lowThreshold) {
        latch.armed = false;
        return true;
    }
    return false;
}

bool upowerCharging(uint32_t state) {
    // 1 Charging, 4 FullyCharged, 5 PendingCharge -> on AC / not discharging.
    return state == 1 || state == 4 || state == 5;
}

bool upowerIsLaptopBattery(uint32_t type, bool isPresent) {
    return isPresent && type == 2; // 2 == Battery.
}

float wivrnChargeToPercent(double charge01) {
    if (charge01 < 0.0) charge01 = 0.0;
    if (charge01 > 1.0) charge01 = 1.0;
    return (float)(charge01 * 100.0);
}

std::string lowBatteryToastText(const std::string& label, float percent) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s battery %d%%", label.c_str(), (int)std::lround(std::max(0.f, percent)));
    return std::string(buf);
}

} // namespace hudbat
