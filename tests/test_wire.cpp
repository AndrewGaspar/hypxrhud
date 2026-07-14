#include "doctest.h"

#include "Wire.hpp"

using namespace hud;

TEST_CASE("wire: round-trips a full upsert (lines + gauges + pose + envelope)") {
    SWireMsg m;
    m.action        = SWireMsg::EAction::Upsert;
    SUpsert& u      = m.upsert;
    u.id            = 7;
    u.owner         = "voice";
    u.slot          = "voice";
    u.space         = "view";
    u.urgency       = 2;
    u.hasPose       = true; u.px = 0.1f; u.py = -0.28f; u.pz = -1.0f;
    u.hasSize       = true; u.sizeW = 0.5f;
    u.fade          = {120, -1, 400, 0.88f};
    u.content.kind  = EPanelKind::Text;
    u.content.confidence = 0.73f;
    u.content.lines = {{"listening", EColor::Accent, true}, {"say “hey hypr”", EColor::Normal, false}};

    std::string wire = Wire::serialize(m);
    CHECK(wire.back() == '\n');

    SWireMsg r;
    REQUIRE(Wire::parse(wire, r));
    CHECK(r.action == SWireMsg::EAction::Upsert);
    const SUpsert& g = r.upsert;
    CHECK(g.id == 7);
    CHECK(g.owner == "voice");
    CHECK(g.slot == "voice");
    CHECK(g.space == "view");
    CHECK(g.urgency == 2);
    CHECK(g.hasPose);
    CHECK(g.px == doctest::Approx(0.1f));
    CHECK(g.py == doctest::Approx(-0.28f));
    CHECK(g.pz == doctest::Approx(-1.0f));
    CHECK(g.hasSize);
    CHECK(g.sizeW == doctest::Approx(0.5f));
    CHECK(g.fade.riseMs == 120);
    CHECK(g.fade.holdMs == -1);
    CHECK(g.fade.fadeMs == 400);
    CHECK(g.fade.opacityCeil == doctest::Approx(0.88f));
    CHECK(g.content.confidence == doctest::Approx(0.73f));
    REQUIRE(g.content.lines.size() == 2);
    CHECK(g.content.lines[0].text == "listening");
    CHECK(g.content.lines[0].color == EColor::Accent);
    CHECK(g.content.lines[0].big);
    CHECK(g.content.lines[1].text == "say “hey hypr”"); // non-ASCII survives
}

TEST_CASE("wire: round-trips a gauges panel") {
    SWireMsg m;
    m.upsert.slot         = "battery";
    m.upsert.content.kind = EPanelKind::Gauges;
    m.upsert.content.gauges = {{"headset", 83.f, true}, {"laptop", 47.f, false}};

    SWireMsg r;
    REQUIRE(Wire::parse(Wire::serialize(m), r));
    REQUIRE(r.upsert.content.kind == EPanelKind::Gauges);
    REQUIRE(r.upsert.content.gauges.size() == 2);
    CHECK(r.upsert.content.gauges[0].label == "headset");
    CHECK(r.upsert.content.gauges[0].percent == doctest::Approx(83.f));
    CHECK(r.upsert.content.gauges[0].charging);
    CHECK(r.upsert.content.gauges[1].label == "laptop");
    CHECK_FALSE(r.upsert.content.gauges[1].charging);
}

TEST_CASE("wire: round-trips a dismiss") {
    SWireMsg m;
    m.action    = SWireMsg::EAction::Dismiss;
    m.dismissId = 42;
    SWireMsg r;
    REQUIRE(Wire::parse(Wire::serialize(m), r));
    CHECK(r.action == SWireMsg::EAction::Dismiss);
    CHECK(r.dismissId == 42);
}

TEST_CASE("wire: malformed input is rejected, not crashed; empty object => defaults") {
    SWireMsg r;
    CHECK_FALSE(Wire::parse("not json", r));
    CHECK_FALSE(Wire::parse("[1,2,3]", r));    // array, not object
    REQUIRE(Wire::parse("{}", r));             // empty object => a default upsert
    CHECK(r.action == SWireMsg::EAction::Upsert);
    CHECK(r.upsert.id == 0);
    CHECK(r.upsert.urgency == 1);
}
