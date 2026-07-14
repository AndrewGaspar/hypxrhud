#include "Theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hud {

namespace {
    std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n\"'");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n\"'");
        return s.substr(a, b - a + 1);
    }
    int hexNybble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
}

const SPalette& defaultPalette() {
    static const SPalette p{};
    return p;
}

SRgb paletteColor(const SPalette& p, EColor c) {
    switch (c) {
        case EColor::Normal: return p.normal;
        case EColor::Dim:    return p.dim;
        case EColor::Accent: return p.accent;
        case EColor::Good:   return p.good;
        case EColor::Warn:   return p.warn;
        case EColor::Bad:    return p.bad;
    }
    return p.normal;
}

SRgb blendRgb(SRgb a, SRgb b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto mix = [&](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(x * (1.f - t) + y * t));
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

bool parseHexColor(const std::string& raw, SRgb& out) {
    std::string s = trim(raw);
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.size() == 3) {
        int r = hexNybble(s[0]), g = hexNybble(s[1]), b = hexNybble(s[2]);
        if (r < 0 || g < 0 || b < 0)
            return false;
        out = {static_cast<uint8_t>(r * 17), static_cast<uint8_t>(g * 17), static_cast<uint8_t>(b * 17)};
        return true;
    }
    if (s.size() == 6 || s.size() == 8) { // RRGGBB or RRGGBBAA (alpha ignored).
        int v[4] = {0, 0, 0, 0};
        for (int i = 0; i < static_cast<int>(s.size()) / 2; i++) {
            int hi = hexNybble(s[i * 2]), lo = hexNybble(s[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return false;
            v[i] = hi * 16 + lo;
        }
        out = {static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2])};
        return true;
    }
    return false;
}

bool parseMakoColors(const std::string& text, SPalette& pal, bool* foundAny) {
    std::istringstream in(text);
    std::string        line;
    bool               haveText = false, haveBg = false, any = false;
    SRgb               textCol = pal.normal, bgCol = pal.panelBg;

    while (std::getline(in, line)) {
        // Strip an inline comment and skip section headers.
        auto hash = line.find('#');
        // NOTE: '#' also starts hex colours, so only treat it as a comment when it is the
        // first non-space char (mako uses ';'/blank sections, not inline '#' comments).
        if (hash != std::string::npos) {
            size_t firstNs = line.find_first_not_of(" \t");
            if (firstNs != std::string::npos && line[firstNs] == '#')
                continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = line.substr(eq + 1);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });

        SRgb col;
        if (key == "text-color") {
            if (parseHexColor(val, col)) { pal.normal = col; textCol = col; haveText = true; any = true; }
        } else if (key == "border-color") {
            if (parseHexColor(val, col)) { pal.accent = col; any = true; }
        } else if (key == "background-color") {
            if (parseHexColor(val, col)) { pal.panelBg = col; bgCol = col; haveBg = true; any = true; }
        }
    }

    // Derive muted hint text + the bar track from the text<->background contrast so they stay
    // legible whichever way the theme leans (light or dark).
    if (haveText && haveBg) {
        pal.dim      = blendRgb(textCol, bgCol, 0.42f); // pull the foreground toward the bg.
        pal.barTrack = blendRgb(bgCol, textCol, 0.28f); // a subtle track on the panel fill.
    }

    if (foundAny)
        *foundAny = any;
    return true;
}

std::string omarchyThemeDir() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.config";
    else
        return "";
    return base + "/omarchy/current/theme";
}

std::string omarchyThemeMakoPath() {
    std::string dir = omarchyThemeDir();
    return dir.empty() ? "" : dir + "/mako.ini";
}

SPalette loadThemePalette(const std::string& path, const SPalette& seed,
                          std::vector<std::string>& warnings) {
    SPalette      pal = seed;
    std::ifstream f(path);
    if (!f) {
        warnings.push_back("theme palette '" + path + "' not found; using the default palette");
        return pal;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    bool any = false;
    parseMakoColors(ss.str(), pal, &any);
    if (!any)
        warnings.push_back("theme palette '" + path + "' had no recognised colour keys; using defaults");
    return pal;
}

SPalette resolvePalette(bool follow, const std::string& themeFile,
                        const std::map<std::string, std::string>& overrides,
                        std::vector<std::string>& warnings) {
    SPalette pal = defaultPalette();

    if (follow) {
        std::string path = themeFile.empty() ? omarchyThemeMakoPath() : themeFile;
        if (!path.empty())
            pal = loadThemePalette(path, pal, warnings);
    }

    // Per-role hex overrides win over the theme.
    for (const auto& [role, hex] : overrides) {
        SRgb col;
        if (!parseHexColor(hex, col)) {
            warnings.push_back("theme override '" + role + "' = '" + hex + "' is not a valid hex colour; ignored");
            continue;
        }
        if (role == "normal")         pal.normal = col;
        else if (role == "dim")       pal.dim = col;
        else if (role == "accent")    pal.accent = col;
        else if (role == "good")      pal.good = col;
        else if (role == "warn")      pal.warn = col;
        else if (role == "bad")       pal.bad = col;
        else if (role == "panel_bg")  pal.panelBg = col;
        else if (role == "bar_track") pal.barTrack = col;
        else warnings.push_back("unknown theme override role '" + role + "'; ignored");
    }
    return pal;
}

} // namespace hud
