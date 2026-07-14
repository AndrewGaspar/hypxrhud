#include "doctest.h"

#include "Theme.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace hud;

namespace {
    // Write `content` to a unique temp file and return its path (removed by the caller / OS).
    std::string writeFixture(const std::string& name, const std::string& content) {
        auto dir  = std::filesystem::temp_directory_path();
        auto path = (dir / ("hypxrhud-" + name)).string();
        std::ofstream f(path);
        f << content;
        f.close();
        return path;
    }

    // The real Forza Horizon theme's mako.ini shape (a light theme): the fixture under test.
    const char* kForzaMako =
        "include=~/.local/share/omarchy/default/mako/core.ini\n"
        "\n"
        "text-color=#16202C\n"
        "border-color=#E6128A\n"
        "background-color=#F4F8FC\n";
}

TEST_CASE("theme: parseHexColor handles #RGB, #RRGGBB, #RRGGBBAA and rejects junk") {
    SRgb c;
    REQUIRE(parseHexColor("#16202C", c));
    CHECK((int)c.r == 0x16);
    CHECK((int)c.g == 0x20);
    CHECK((int)c.b == 0x2C);

    REQUIRE(parseHexColor("#f0a", c)); // short form expands each nibble.
    CHECK((int)c.r == 0xFF);
    CHECK((int)c.g == 0x00);
    CHECK((int)c.b == 0xAA);

    REQUIRE(parseHexColor("#E6128AFF", c)); // trailing alpha is parsed then dropped.
    CHECK((int)c.r == 0xE6);
    CHECK((int)c.b == 0x8A);

    REQUIRE(parseHexColor("16202C", c)); // leading '#' optional.
    CHECK((int)c.g == 0x20);

    CHECK_FALSE(parseHexColor("#12", c));
    CHECK_FALSE(parseHexColor("#gggggg", c));
    CHECK_FALSE(parseHexColor("", c));
}

TEST_CASE("theme: parseMakoColors maps text/border/background and derives dim + bar track") {
    SPalette pal = defaultPalette();
    bool     any = false;
    REQUIRE(parseMakoColors(kForzaMako, pal, &any));
    CHECK(any);

    // text-color -> normal ; border-color -> accent ; background-color -> panel fill.
    CHECK((int)pal.normal.r == 0x16);
    CHECK((int)pal.normal.g == 0x20);
    CHECK((int)pal.normal.b == 0x2C);
    CHECK((int)pal.accent.r == 0xE6);
    CHECK((int)pal.accent.g == 0x12);
    CHECK((int)pal.accent.b == 0x8A);
    CHECK((int)pal.panelBg.r == 0xF4);
    CHECK((int)pal.panelBg.b == 0xFC);

    // dim is derived BETWEEN the (dark) text and the (light) background — muted, not either.
    CHECK((int)pal.dim.r > (int)pal.normal.r);
    CHECK((int)pal.dim.r < (int)pal.panelBg.r);

    // status roles are not expressed in mako.ini, so they keep the tuned defaults.
    CHECK((int)pal.good.g == (int)defaultPalette().good.g);
    CHECK((int)pal.bad.r == (int)defaultPalette().bad.r);
}

TEST_CASE("theme: parseMakoColors ignores include/unknown keys and reports foundAny=false") {
    SPalette pal = defaultPalette();
    bool     any = true;
    const char* onlyNoise =
        "include=~/.local/share/omarchy/default/mako/core.ini\n"
        "anchor=top-right\n"
        "default-timeout=5000\n";
    REQUIRE(parseMakoColors(onlyNoise, pal, &any));
    CHECK_FALSE(any);
    // Untouched: equals the default palette.
    CHECK((int)pal.normal.r == (int)defaultPalette().normal.r);
    CHECK((int)pal.accent.b == (int)defaultPalette().accent.b);
}

TEST_CASE("theme: loadThemePalette reads a fixture; a missing file leaves the seed intact") {
    std::vector<std::string> warns;

    std::string path = writeFixture("theme-fixture.ini", kForzaMako);
    SPalette    got  = loadThemePalette(path, defaultPalette(), warns);
    CHECK((int)got.accent.r == 0xE6);
    CHECK(warns.empty());
    std::remove(path.c_str());

    warns.clear();
    SPalette missing = loadThemePalette("/nonexistent/hypxrhud/mako.ini", defaultPalette(), warns);
    CHECK((int)missing.normal.r == (int)defaultPalette().normal.r);
    CHECK_FALSE(warns.empty()); // a note, not a hard error.
}

TEST_CASE("theme: resolvePalette — follow off gives defaults; overrides win; bad hex warns") {
    std::vector<std::string> warns;

    // follow=false: no theme file read, the default palette is returned.
    SPalette def = resolvePalette(false, "", {}, warns);
    CHECK((int)def.normal.r == (int)defaultPalette().normal.r);

    // Per-role overrides win over everything (applied even with follow off).
    warns.clear();
    std::map<std::string, std::string> ov = {{"accent", "#00FF00"}, {"panel_bg", "#101010"}};
    SPalette o = resolvePalette(false, "", ov, warns);
    CHECK((int)o.accent.g == 0xFF);
    CHECK((int)o.accent.r == 0x00);
    CHECK((int)o.panelBg.r == 0x10);
    CHECK(warns.empty());

    // A malformed override is skipped with a warning, the rest still apply.
    warns.clear();
    SPalette b = resolvePalette(false, "", {{"good", "not-a-color"}}, warns);
    CHECK((int)b.good.g == (int)defaultPalette().good.g);
    CHECK_FALSE(warns.empty());
}

TEST_CASE("theme: a fixture theme file layered under resolvePalette themes normal + accent") {
    std::string path = writeFixture("resolve-fixture.ini", kForzaMako);
    std::vector<std::string> warns;
    SPalette pal = resolvePalette(/*follow=*/true, path, {}, warns);
    CHECK((int)pal.normal.r == 0x16);
    CHECK((int)pal.accent.r == 0xE6);
    std::remove(path.c_str());
}
