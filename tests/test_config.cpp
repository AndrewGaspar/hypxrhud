#include "doctest.h"

#include "Config.hpp"
#include "Slots.hpp"

using namespace hud;

TEST_CASE("config: [hud] keys parse into the struct") {
    const char* text =
        "[hud]\n"
        "z = 25\n"
        "gpu = \"/dev/dri/renderD128\"\n"
        "opacity = 0.8\n"
        "blend_mode = \"alpha\"\n"
        "per_client_cap = 6\n"
        "tex_w = 1024\n"
        "tex_h = 512\n"
        "rise_ms = 90\n"
        "hold_ms = 3000\n"
        "fade_ms = 300\n";
    SConfig cfg;
    std::vector<std::string> e, w;
    REQUIRE(parseConfig(text, cfg, e, w));
    CHECK(cfg.hudZ == 25);
    CHECK(cfg.gpu == "/dev/dri/renderD128");
    CHECK(cfg.opacity == doctest::Approx(0.8f));
    CHECK(cfg.blendMode == "alpha");
    CHECK(cfg.perClientCap == 6);
    CHECK(cfg.texW == 1024);
    CHECK(cfg.texH == 512);
    CHECK(cfg.riseMs == 90);
    CHECK(cfg.holdMs == 3000);
    CHECK(cfg.fadeMs == 300);
}

TEST_CASE("config: per-slot overrides parse and apply to the registry") {
    const char* text =
        "[slot.voice]\n"
        "pose = \"0.1,-0.3,-1.5\"\n"
        "size = 0.5\n"
        "space = \"local\"\n"
        "[slot.toast]\n"
        "max = 5\n";
    SConfig cfg;
    std::vector<std::string> e, w;
    REQUIRE(parseConfig(text, cfg, e, w));

    CSlots slots;
    cfg.applySlots(slots);
    const SSlot* v = slots.find("voice");
    REQUIRE(v);
    CHECK(v->px == doctest::Approx(0.1f));
    CHECK(v->py == doctest::Approx(-0.3f));
    CHECK(v->pz == doctest::Approx(-1.5f));
    CHECK(v->sizeW == doctest::Approx(0.5f));
    CHECK(v->space == "local");
    CHECK(slots.find("toast")->maxStack == 5);
}

TEST_CASE("config: unknown keys warn but do not fail; bad enum errors") {
    SConfig cfg;
    std::vector<std::string> e, w;
    REQUIRE(parseConfig("[hud]\nbogus = 3\n", cfg, e, w));
    CHECK(e.empty());
    CHECK_FALSE(w.empty());

    e.clear(); w.clear();
    CHECK_FALSE(parseConfig("[hud]\nblend_mode = \"rainbow\"\n", cfg, e, w));
    CHECK_FALSE(e.empty());
}

TEST_CASE("config: [theme] follow/file/overrides parse (WP-H6)") {
    const char* text =
        "[theme]\n"
        "follow = false\n"
        "file = \"/some/theme/mako.ini\"\n"
        "accent = \"#00FF00\"\n"
        "panel_bg = \"#101010\"\n";
    SConfig cfg;
    std::vector<std::string> e, w;
    REQUIRE(parseConfig(text, cfg, e, w));
    CHECK(cfg.themeFollow == false);
    CHECK(cfg.themeFile == "/some/theme/mako.ini");
    REQUIRE(cfg.colorOverrides.count("accent"));
    CHECK(cfg.colorOverrides.at("accent") == "#00FF00");
    CHECK(cfg.colorOverrides.at("panel_bg") == "#101010");

    // A bare boolean is also accepted.
    SConfig cfg2;
    e.clear(); w.clear();
    REQUIRE(parseConfig("[theme]\nfollow = true\n", cfg2, e, w));
    CHECK(cfg2.themeFollow == true);
}

TEST_CASE("config: slot on_refuse=queue applies to the registry (WP-H5)") {
    const char* text =
        "[slot.voice]\n"
        "on_refuse = \"queue\"\n"
        "[slot.keys]\n"
        "on_refuse = \"refuse\"\n";
    SConfig cfg;
    std::vector<std::string> e, w;
    REQUIRE(parseConfig(text, cfg, e, w));

    CSlots slots;
    cfg.applySlots(slots);
    CHECK(slots.find("voice")->onRefuse == ERefusePolicy::Queue);
    CHECK(slots.find("keys")->onRefuse == ERefusePolicy::Refuse);
    // Untouched slots keep the default (refuse).
    CHECK(slots.find("toast")->onRefuse == ERefusePolicy::Refuse);

    // A bad value errors.
    SConfig bad;
    e.clear(); w.clear();
    CHECK_FALSE(parseConfig("[slot.voice]\non_refuse = \"maybe\"\n", bad, e, w));
    CHECK_FALSE(e.empty());
}

TEST_CASE("config: a missing file yields defaults, not an error") {
    SConfig cfg;
    std::vector<std::string> e, w;
    CHECK(loadConfigFile("/nonexistent/hypxrhud.toml", cfg, e, w));
    CHECK(e.empty());
    CHECK(cfg.hudZ == 20); // untouched default
}
