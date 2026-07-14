#pragma once

#include "Slots.hpp"

#include <map>
#include <string>
#include <vector>

// hypxrhud config — a DELIBERATELY THIN TOML loader (triage #6): one loader, one
// struct, so a Lua front end can swap in behind the same struct later (the whole family
// converges on Lua once Omarchy's Lua story stabilises). Nothing consuming the config
// knows the format — it reads SConfig. Mirrors hypxrvoice's Config.cpp conventions
// (a TOML subset: sections, key = value, quoted strings, #comments).
//
// Keys (design memo §1/§4/§5):
//   [hud]
//     z            = 20        # overlay sessionLayersPlacement (above HypXRland monitors)
//     gpu          = "/dev/dri/renderD128"  # DRM render node; empty = auto (match runtime)
//     opacity      = 0.92      # default per-panel opacity ceiling
//     blend_mode   = "auto"    # auto | opaque | alpha | additive (auto: prefer alpha over passthrough, memo §6.3)
//     per_client_cap = 4       # max panels per client (triage #9)
//     tex_w = 768              # per-panel swapchain/raster width
//     tex_h = 384              # per-panel swapchain/raster height
//     rise_ms = 110 / hold_ms = 2600 / fade_ms = 450   # default fade envelope
//   [theme]                    # WP-H6 palette (Omarchy current theme)
//     follow    = true         # follow ~/.config/omarchy/current/theme (default true)
//     file      = ""           # override palette file path (empty = auto: theme mako.ini)
//     normal/dim/accent/good/warn/bad/panel_bg/bar_track = "#RRGGBB"  # per-role overrides
//   [slot.<name>]              # override any of the six slots
//     pose      = "0,-0.28,-1.0" # centre, metres
//     size      = 0.42           # quad width, metres
//     space     = "view"         # view | local
//     max       = 3              # stack cap (toast)
//     on_refuse = "refuse"       # singleton contention: refuse | queue (WP-H5)
//     opacity   = 0.9            # per-slot opacity ceiling override (reserved)

namespace hud {

struct SConfig {
    int         hudZ         = 20;
    std::string gpu;                       // empty = auto-scan render node.
    float       opacity      = 0.92f;      // default opacity ceiling.
    std::string blendMode    = "auto";     // auto | opaque | alpha | additive (auto: prefer alpha for the overlay).
    int         perClientCap = 4;
    int         texW = 768, texH = 384;
    int         riseMs = 110, holdMs = 2600, fadeMs = 450;

    // WP-H4 runtime re-probe backoff (memo §6.1): grow base->cap while the runtime is
    // absent; a gentle fixed `base` cadence while the headset is merely undonned.
    int         reprobeBaseMs = 2000;
    int         reprobeCapMs  = 30000;

    // WP-H6 theming ([theme]). `themeFollow` gates sourcing colours from the Omarchy current
    // theme; `themeFile` overrides which palette file is read (empty = the theme's mako.ini);
    // `colorOverrides` maps a role name -> hex string, applied last (wins over the theme).
    bool                               themeFollow = true;
    std::string                        themeFile;
    std::map<std::string, std::string> colorOverrides;

    struct SSlotOverride {
        bool  hasPose = false; float px = 0, py = 0, pz = -1;
        bool  hasSize = false; float sizeW = 0.42f;
        bool  hasSpace = false; std::string space;
        bool  hasMax = false;  int max = 3;
        bool  hasRefuse = false; bool refuseQueue = false; // WP-H5: singleton on_refuse=queue.
        bool  hasOpacity = false; float opacity = 0.92f;
    };
    std::map<std::string, SSlotOverride> slots;

    // Apply the per-slot overrides onto a slot registry (called after load).
    void applySlots(CSlots& registry) const;
};

// Parse a TOML-subset document. Unknown keys are warnings (forward-compat); hard type
// errors fail the parse. Pure over a string -> directly unit-testable.
bool parseConfig(const std::string& text, SConfig& out, std::vector<std::string>& errors,
                 std::vector<std::string>& warnings);

// Load from a path. A missing file is NOT an error (defaults returned, noted in
// warnings). Returns false only on a parse error.
bool loadConfigFile(const std::string& path, SConfig& out, std::vector<std::string>& errors,
                    std::vector<std::string>& warnings);

// $XDG_CONFIG_HOME/hypxrhud/hypxrhud.toml (or ~/.config/...).
std::string defaultConfigPath();

} // namespace hud
