#pragma once

#include "BatteryModel.hpp"

// hypxrhud-battery — the THIN sd-bus source adapters. All the semantics live in the pure
// BatteryModel; these functions only read D-Bus properties into an SSourceReading and hand
// off to the pure parsers. Each degrades cleanly: a missing service, object, or property
// yields `present=false` (the gauge is omitted) rather than an error.

struct sd_bus;

namespace hudbat {

// ---- UPower laptop battery (system bus) -----------------------------------------------
// Reads org.freedesktop.UPower's aggregate DisplayDevice
// (/org/freedesktop/UPower/devices/DisplayDevice): Percentage(d), State(u), Type(u),
// IsPresent(b). Present iff Type==Battery && IsPresent (a desktop's DisplayDevice is not a
// battery -> absent). Returns a fully-populated reading; on ANY read failure returns an
// absent reading (UPower not installed / no battery).
inline constexpr const char* kUPowerBus  = "org.freedesktop.UPower";
inline constexpr const char* kUPowerPath = "/org/freedesktop/UPower/devices/DisplayDevice";
inline constexpr const char* kUPowerDev  = "org.freedesktop.UPower.Device";

SSourceReading readUpower(sd_bus* system);

// ---- WiVRn headset battery (session bus) ----------------------------------------------
// Reads io.github.wivrn.Server. `HeadsetConnected`(b) tells us a headset is attached.
//
// The headset CHARGE itself is NOT on WiVRn's D-Bus interface as of v26.6.1 (see
// docs/battery-wivrn.md): the value lives only inside the Monado HMD xrt_device
// (get_battery_status) with no external/OpenXR read path. This reader consumes a forward-
// compatible `Battery` property so it lights up automatically once WiVRn exposes it (the
// documented seam / minimal patch): it tries signature `(bbd)` = (present, charging,
// charge[0..1]) first, then a bare `d` (charge only). Until that property exists the read
// returns present=false and the headset gauge is cleanly omitted, even while a headset is
// connected — never a stale or fabricated value.
inline constexpr const char* kWivrnBus  = "io.github.wivrn.Server";
inline constexpr const char* kWivrnPath = "/io/github/wivrn/Server";
inline constexpr const char* kWivrnIface = "io.github.wivrn.Server";

SSourceReading readWivrn(sd_bus* session);

// True iff a headset is currently connected (independent of whether its charge is readable)
// — used only for diagnostics / logging.
bool wivrnHeadsetConnected(sd_bus* session);

} // namespace hudbat
