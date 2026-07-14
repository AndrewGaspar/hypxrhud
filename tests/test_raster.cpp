#include "doctest.h"

#include "PanelText.hpp"

using namespace hud;

namespace {
    uint32_t opaqueish(const SImage& img, uint8_t thresh = 40) {
        uint32_t n = 0;
        for (size_t i = 3; i < img.rgba.size(); i += 4)
            if (img.rgba[i] > thresh)
                n++;
        return n;
    }
}

TEST_CASE("raster: empty content is a fully transparent image of the right size") {
    SPanelContent empty;
    SImage img = renderPanel(empty, 256, 128);
    CHECK(img.w == 256);
    CHECK(img.h == 128);
    REQUIRE(img.rgba.size() == 256u * 128u * 4u);
    uint32_t alphaSum = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        alphaSum += img.rgba[i];
    CHECK(alphaSum == 0u);
}

TEST_CASE("raster: a text panel draws visible pixels") {
    SPanelContent c;
    c.lines = {{"anchoring XR-code", EColor::Accent, true}, {"you looked at it", EColor::Dim, false}};
    c.confidence = 0.8f;
    SImage img = renderPanel(c, 768, 384);
    REQUIRE(!img.empty());
    CHECK(opaqueish(img) > 500u);
}

TEST_CASE("raster: a dual-gauge battery panel draws visible pixels") {
    SPanelContent c;
    c.kind   = EPanelKind::Gauges;
    c.lines  = {{"battery", EColor::Dim, false}};
    c.gauges = {{"headset", 83.f, true}, {"laptop", 47.f, false}};
    SImage img = renderPanel(c, 768, 384);
    REQUIRE(!img.empty());
    // Two gauge bars + label rows + title cover a meaningful area.
    CHECK(opaqueish(img) > 800u);
}

TEST_CASE("raster: an unknown-percent gauge still renders (drawn as a dash)") {
    SPanelContent c;
    c.kind   = EPanelKind::Gauges;
    c.gauges = {{"laptop", -1.f, false}};
    SImage img = renderPanel(c, 512, 256);
    CHECK(!img.empty());
    CHECK(opaqueish(img) > 100u);
}
