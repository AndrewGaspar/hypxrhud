#pragma once

#include <string>
#include <vector>

// hypxrhud-battery config — the same DELIBERATELY THIN TOML subset the daemon's Config.cpp
// uses (sections, key = value, quoted strings, # comments), one struct so a Lua front end
// can swap in later behind it. The client reads its OWN file
// ($XDG_CONFIG_HOME/hypxrhud/battery.toml) so it can run standalone / bus-activated without
// the daemon's config — all keys live under a single [battery] section.
//
//   [battery]
//     poll_interval_sec = 30      # source poll cadence; PropertiesChanged also wakes early
//     low_threshold     = 15      # percent at/below which a one-shot toast fires
//     low_hysteresis    = 5       # percent above the threshold that re-arms the one-shot
//     headset           = true    # show the WiVRn headset gauge
//     laptop            = true    # show the UPower laptop gauge
//     headset_label     = "headset"
//     laptop_label      = "laptop"
//     slot              = "battery"   # the HUD slot the panel targets
//     toasts            = true    # post low-battery toasts

namespace hudbat {

struct SBatteryConfig {
    int         pollIntervalSec = 30;
    int         lowThreshold    = 15;
    int         lowHysteresis   = 5;
    bool        showHeadset     = true;
    bool        showLaptop      = true;
    std::string headsetLabel    = "headset";
    std::string laptopLabel     = "laptop";
    std::string slot            = "battery";
    bool        toasts          = true;
};

// Parse a TOML-subset document. Unknown keys/sections are warnings (forward-compat); hard
// type errors fail the parse. Pure over a string -> directly unit-testable.
bool parseBatteryConfig(const std::string& text, SBatteryConfig& out,
                        std::vector<std::string>& errors, std::vector<std::string>& warnings);

// Load from a path. A missing file is NOT an error (defaults returned, noted in warnings).
bool loadBatteryConfigFile(const std::string& path, SBatteryConfig& out,
                           std::vector<std::string>& errors, std::vector<std::string>& warnings);

// $XDG_CONFIG_HOME/hypxrhud/battery.toml (or ~/.config/hypxrhud/battery.toml).
std::string defaultBatteryConfigPath();

} // namespace hudbat
