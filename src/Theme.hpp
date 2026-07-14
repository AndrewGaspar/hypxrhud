#pragma once

#include "Panel.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// hypxrhud — the PURE theming palette (WP-H6). The rasteriser (PanelText) maps each
// semantic EColor role to a concrete RGBA; that mapping is the single theming seam. This
// file makes the palette a VALUE the renderer is handed (no globals), so it is pure and
// unit-testable, and so --preview / the daemon can swap it per Omarchy theme.
//
// Palette source (design memo §5.2): Omarchy exposes the CURRENT theme under the symlink
// `~/.config/omarchy/current/theme/`, whose `mako.ini` is the semantically closest colour
// file — it carries `text-color` / `border-color` / `background-color`. We map those onto
// the foreground/accent/panel-background roles and DERIVE the muted `dim` + the bar track
// from the text<->background contrast. The status roles (good/warn/bad) are not expressed
// in mako.ini, so they keep the tuned defaults. When no Omarchy theme is present the
// hardcoded default palette (the pre-WP-H6 constants) is used verbatim.
//
// Live reload is handled outside this file (ThemeWatch, daemon-only): an inotify watch on
// the theme symlink's parent re-resolves the palette and force-dirties every panel.

namespace hud {

// A concrete 8-bit RGB colour. Shared with the rasteriser (PanelText).
struct SRgb {
    uint8_t r = 0, g = 0, b = 0;
};

// The full concrete palette the rasteriser consumes. Defaults ARE the pre-WP-H6 hardcoded
// constants, so an un-themed build renders byte-identically to before.
struct SPalette {
    SRgb  normal   {235, 238, 242}; // primary foreground / body text.
    SRgb  dim      {150, 158, 168}; // secondary / hint text.
    SRgb  accent   {120, 190, 255}; // the verb / focus / highlight.
    SRgb  good      {120, 220, 150}; // success / high confidence / charging.
    SRgb  warn      {250, 200, 110}; // caution / low confidence / low battery.
    SRgb  bad       {255, 120, 120}; // error / critical.
    SRgb  panelBg   {13, 15, 20};    // the rounded panel fill behind the content.
    float panelAlpha = 0.66f;        // panel-fill alpha.
    SRgb  barTrack   {64, 69, 77};   // confidence / gauge track (the unfilled part).
};

// The default (un-themed) palette — the fallback when no Omarchy theme is present.
const SPalette& defaultPalette();

// Semantic role -> concrete colour.
SRgb paletteColor(const SPalette& p, EColor c);

// Linear blend a->b by t in [0,1]. Exposed for the derivations + tests.
SRgb blendRgb(SRgb a, SRgb b, float t);

// Parse a hex colour: #RGB, #RRGGBB or #RRGGBBAA (alpha parsed but not stored). Leading
// '#' optional; surrounding whitespace/quotes tolerated. Returns false on a malformed value.
bool parseHexColor(const std::string& s, SRgb& out);

// Parse a mako.ini-style colours file (a `key=value` subset; `include=` and unrelated keys
// ignored). Seeds `pal` with its incoming values (pass defaultPalette()) and overrides:
//   text-color        -> normal
//   border-color      -> accent
//   background-color  -> panelBg
// When BOTH text and background are present, `dim` and `barTrack` are re-derived from their
// contrast so hint text + bar tracks stay legible on the themed background. Unknown keys are
// ignored (forward-compatible). Always returns true (a file with no colour keys just leaves
// the seed untouched); `foundAny` reports whether any colour was applied.
bool parseMakoColors(const std::string& text, SPalette& pal, bool* foundAny = nullptr);

// ---- Omarchy theme resolution -------------------------------------------------------

// Absolute path to `~/.config/omarchy/current/theme` (honours $XDG_CONFIG_HOME), or "" if
// $HOME/$XDG_CONFIG_HOME are unset. The path is returned whether or not it exists.
std::string omarchyThemeDir();

// The mako.ini inside the current theme dir (omarchyThemeDir()+"/mako.ini"), or "".
std::string omarchyThemeMakoPath();

// Read a palette file from `path` on top of `seed`. A missing file is NOT an error (the
// seed is returned and a note is appended to `warnings`).
SPalette loadThemePalette(const std::string& path, const SPalette& seed,
                          std::vector<std::string>& warnings);

// The full resolution used by the daemon + --preview:
//   1. start from defaultPalette();
//   2. if follow is on, layer the theme file (cfg.themeFile, else the Omarchy mako.ini);
//   3. apply per-role hex overrides (they WIN over the theme).
// `overrides` maps a role name (normal/dim/accent/good/warn/bad/panel_bg/bar_track) to a hex
// string; a malformed override is skipped with a warning. Pure over its inputs (only the
// theme-file read touches the filesystem).
SPalette resolvePalette(bool follow, const std::string& themeFile,
                        const std::map<std::string, std::string>& overrides,
                        std::vector<std::string>& warnings);

} // namespace hud
