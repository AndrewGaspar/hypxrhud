#pragma once

#include "Panel.hpp" // hud::SGauge

#include <cstdint>
#include <string>
#include <vector>

// hypxrhud-battery — the PURE battery model. No sd-bus, no I/O: just the value semantics
// the client's thin bus adapters feed into and the panel content they build out. Kept pure
// so the interesting logic (source parsing, the gauge diff / zero-cost-when-static
// contract, and the one-shot low-battery latch) is unit-tested with plain fixtures and no
// live bus (tests/test_battery.cpp).
//
// TWO sources feed the one `battery` slot (user decision):
//   - the HEADSET battery, via WiVRn (see WivrnSource.*)
//   - the LAPTOP battery, via UPower's DisplayDevice (see UpowerSource.*)
// Each is read into an SSourceReading; an absent source (no headset connected, a desktop
// with no battery, or a value we cannot obtain) is `present=false` and is OMITTED from the
// panel entirely — never drawn as a stale gauge.

namespace hudbat {

// One source's current state. `present` gates whether the gauge is shown at all; a present
// source with an unknown percentage (percent < 0) is still omitted by buildGauges (we never
// show a value we do not have). Charging drives the palette (green) and re-arms the latch.
struct SSourceReading {
    bool  present  = false;
    float percent  = -1.f; // [0,100]; < 0 = unknown.
    bool  charging = false;
};

// The static configuration the model needs (a slice of SBatteryConfig, passed pure).
struct SModelParams {
    bool        showHeadset  = true;
    bool        showLaptop   = true;
    std::string headsetLabel = "headset";
    std::string laptopLabel  = "laptop";
    int         lowThreshold = 15; // percent at/below which a low-battery toast fires.
    int         lowHysteresis = 5; // percent above the threshold that re-arms the one-shot.
};

// Build the panel's gauge list from the two readings. A source is included iff its `show`
// flag is set AND it is present AND its percent is known (>= 0). Order is deterministic:
// headset first, then laptop. Percent is clamped to [0,100].
std::vector<hud::SGauge> buildGauges(const SSourceReading& headset,
                                     const SSourceReading& laptop,
                                     const SModelParams&   p);

// Whether two gauge lists are equal for the purpose of the UpdatePanel diff. Percent is
// compared at 1% granularity (rounded) so sub-percent jitter never re-sends a frame; label,
// charging, and presence/order all matter. This IS the zero-cost-when-static contract: the
// client only calls UpdatePanel when this returns false.
bool gaugesEqual(const std::vector<hud::SGauge>& a, const std::vector<hud::SGauge>& b);

// One-shot low-battery latch (per source, per discharge cycle). `armed` starts true. The
// latch FIRES (returns true, and disarms) when the source is present, discharging, and at or
// below `lowThreshold`. It RE-ARMS (so the next dip fires again) once the source is charging
// or has risen to `lowThreshold + lowHysteresis` or above. A vanished/unknown source leaves
// the latch untouched (neither fires nor re-arms) so unplugging the headset mid-warning does
// not spuriously re-fire when it returns. Returns true exactly on the firing transition.
struct SLatch {
    bool armed = true;
};
bool lowBatteryLatch(SLatch& latch, const SSourceReading& r, int lowThreshold, int lowHysteresis);

// ---- source value parsing (pure, fixture-tested) -------------------------------------

// UPower Device.State -> our charging flag. State enum (org.freedesktop.UPower.Device):
//   1 Charging, 2 Discharging, 3 Empty, 4 FullyCharged, 5 PendingCharge, 6 PendingDischarge.
// On AC (Charging/FullyCharged/PendingCharge) we show the charging (green) state.
bool upowerCharging(uint32_t state);

// UPower Device.Type == 2 is Battery; anything else (Unknown/LinePower/UPS/...) means the
// DisplayDevice is not a laptop battery (a desktop) -> not present.
bool upowerIsLaptopBattery(uint32_t type, bool isPresent);

// WiVRn headset battery: charge is a fraction [0,1] (from_headset::battery.charge, computed
// on the client as level/scale). Returns percent [0,100]; a charge outside [0,1] clamps.
float wivrnChargeToPercent(double charge01);

// Format the one-shot low-battery toast line, e.g. "headset battery 14%".
std::string lowBatteryToastText(const std::string& label, float percent);

} // namespace hudbat
