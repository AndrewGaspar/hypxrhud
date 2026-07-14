#include "doctest.h"

#include "Props.hpp"

using namespace hud;

namespace {
    SConfig defcfg() {
        SConfig c; // riseMs=110, holdMs=2600, fadeMs=450, opacity=0.92 by default
        return c;
    }
}

TEST_CASE("props: empty map -> a default create upsert (id/owner from context)") {
    SUpsert u = upsertFromProps(0, ":1.42", {}, defcfg());
    CHECK(u.id == 0);
    CHECK(u.owner == ":1.42");
    CHECK(u.slot.empty());
    CHECK(u.urgency == 1);
    CHECK_FALSE(u.hasPose);
    CHECK_FALSE(u.hasSize);
    // Envelope inherits config defaults.
    CHECK(u.fade.riseMs == 110);
    CHECK(u.fade.holdMs == 2600);
    CHECK(u.fade.fadeMs == 450);
    CHECK(u.fade.opacityCeil == doctest::Approx(0.92f));
    CHECK(u.content.kind == EPanelKind::Text);
    CHECK(u.content.lines.empty());
}

TEST_CASE("props: a full text panel maps every field 1:1 with SUpsert") {
    SPropMap p;
    p["slot"]       = SPropValue::str("voice");
    p["space"]      = SPropValue::str("view");
    p["urgency"]    = SPropValue::integer(2);
    p["pose"]       = SPropValue::vec3(0.1f, -0.28f, -1.0f);
    p["size"]       = SPropValue::dbl(0.5);
    p["rise_ms"]    = SPropValue::integer(120);
    p["hold_ms"]    = SPropValue::integer(-1);
    p["fade_ms"]    = SPropValue::integer(400);
    p["opacity"]    = SPropValue::dbl(0.88);
    p["confidence"] = SPropValue::dbl(0.73);
    p["lines"]      = SPropValue::lineList({{"listening", EColor::Accent, true},
                                            {"open the browser", EColor::Normal, false}});

    SUpsert u = upsertFromProps(7, ":1.5", p, defcfg());
    CHECK(u.id == 7);
    CHECK(u.owner == ":1.5");
    CHECK(u.slot == "voice");
    CHECK(u.space == "view");
    CHECK(u.urgency == 2);
    CHECK(u.hasPose);
    CHECK(u.px == doctest::Approx(0.1f));
    CHECK(u.py == doctest::Approx(-0.28f));
    CHECK(u.pz == doctest::Approx(-1.0f));
    CHECK(u.hasSize);
    CHECK(u.sizeW == doctest::Approx(0.5f));
    CHECK(u.fade.riseMs == 120);
    CHECK(u.fade.holdMs == -1);
    CHECK(u.fade.fadeMs == 400);
    CHECK(u.fade.opacityCeil == doctest::Approx(0.88f));
    CHECK(u.content.confidence == doctest::Approx(0.73f));
    REQUIRE(u.content.lines.size() == 2);
    CHECK(u.content.lines[0].text == "listening");
    CHECK(u.content.lines[0].color == EColor::Accent);
    CHECK(u.content.lines[0].big);
    CHECK(u.content.lines[1].text == "open the browser");
}

TEST_CASE("props: urgency accepts any numeric variant (y/u/i/x/d)") {
    SPropMap p;
    p["urgency"] = SPropValue::boolean(true); // y=1 style
    CHECK(upsertFromProps(0, "o", p, defcfg()).urgency == 1);
    p["urgency"] = SPropValue::dbl(2.0);
    CHECK(upsertFromProps(0, "o", p, defcfg()).urgency == 2);
    p["urgency"] = SPropValue::integer(0);
    CHECK(upsertFromProps(0, "o", p, defcfg()).urgency == 0);
}

TEST_CASE("props: title is a convenience big Accent line, prepended before lines") {
    SPropMap p;
    p["title"] = SPropValue::str("Battery low");
    p["lines"] = SPropValue::lineList({{"12% remaining", EColor::Warn, false}});
    SUpsert u  = upsertFromProps(0, "o", p, defcfg());
    REQUIRE(u.content.lines.size() == 2);
    CHECK(u.content.lines[0].text == "Battery low");
    CHECK(u.content.lines[0].big);
    CHECK(u.content.lines[0].color == EColor::Accent);
    CHECK(u.content.lines[1].text == "12% remaining");
}

TEST_CASE("props: gauges with no explicit kind become a gauges panel") {
    SPropMap p;
    p["slot"]   = SPropValue::str("battery");
    p["gauges"] = SPropValue::gaugeList({{"headset", 83.f, true}, {"laptop", 47.f, false}});
    SUpsert u   = upsertFromProps(0, "o", p, defcfg());
    CHECK(u.content.kind == EPanelKind::Gauges);
    REQUIRE(u.content.gauges.size() == 2);
    CHECK(u.content.gauges[0].label == "headset");
    CHECK(u.content.gauges[0].percent == doctest::Approx(83.f));
    CHECK(u.content.gauges[0].charging);
    CHECK(u.content.gauges[1].charging == false);
}

TEST_CASE("props: explicit kind is honoured over the gauges heuristic") {
    SPropMap p;
    p["kind"]   = SPropValue::str("text");
    p["gauges"] = SPropValue::gaugeList({{"headset", 83.f, true}});
    p["lines"]  = SPropValue::lineList({{"hi", EColor::Normal, false}});
    SUpsert u   = upsertFromProps(0, "o", p, defcfg());
    CHECK(u.content.kind == EPanelKind::Text);
}

TEST_CASE("props: unknown keys are ignored (forward-compatible)") {
    SPropMap p;
    p["slot"]           = SPropValue::str("toast");
    p["future_feature"] = SPropValue::str("whatever");
    p["another"]        = SPropValue::integer(99);
    SUpsert u           = upsertFromProps(0, "o", p, defcfg());
    CHECK(u.slot == "toast"); // recognised key still applied; unknowns dropped silently
}

TEST_CASE("props: size accepts an integer variant too") {
    SPropMap p;
    p["size"] = SPropValue::integer(1);
    SUpsert u = upsertFromProps(0, "o", p, defcfg());
    CHECK(u.hasSize);
    CHECK(u.sizeW == doctest::Approx(1.0f));
}
