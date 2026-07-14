#include "doctest.h"

#include "Panel.hpp"

using namespace hud;

TEST_CASE("panel: colour role names round-trip") {
    for (EColor c : {EColor::Normal, EColor::Dim, EColor::Accent, EColor::Good, EColor::Warn, EColor::Bad})
        CHECK(colorFromName(colorName(c)) == c);
    CHECK(colorFromName("nonsense") == EColor::Normal); // unknown -> Normal
}

TEST_CASE("panel: kind names round-trip") {
    CHECK(panelKindFromName(panelKindName(EPanelKind::Text)) == EPanelKind::Text);
    CHECK(panelKindFromName(panelKindName(EPanelKind::Gauges)) == EPanelKind::Gauges);
    CHECK(panelKindFromName("nonsense") == EPanelKind::Text); // unknown -> Text
}

TEST_CASE("panel: empty() reflects kind-appropriate content") {
    SPanelContent text;
    CHECK(text.empty());
    text.lines.push_back({"hi", EColor::Normal, false});
    CHECK_FALSE(text.empty());

    SPanelContent gauges;
    gauges.kind = EPanelKind::Gauges;
    CHECK(gauges.empty());
    gauges.gauges.push_back({"headset", 80.f, true});
    CHECK_FALSE(gauges.empty());
}
