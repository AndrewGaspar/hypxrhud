#include "doctest.h"

#include "Preview.hpp"
#include "Scene.hpp"

using namespace hud;

TEST_CASE("preview: the demo scene fills all six slots, battery has two gauges") {
    CScene scene;
    buildPreviewScene(scene, 1000);
    CHECK(scene.panels().size() == 6);

    int seenSlots = 0;
    const SPanel* battery = nullptr;
    for (auto& [id, p] : scene.panels()) {
        if (p.slot == "voice" || p.slot == "keys" || p.slot == "toast" ||
            p.slot == "status" || p.slot == "media" || p.slot == "battery")
            seenSlots++;
        if (p.slot == "battery")
            battery = &p;
    }
    CHECK(seenSlots == 6);
    REQUIRE(battery != nullptr);
    CHECK(battery->content.kind == EPanelKind::Gauges);
    REQUIRE(battery->content.gauges.size() == 2);
    CHECK(battery->content.gauges[0].label == "headset");
    CHECK(battery->content.gauges[1].label == "laptop");
}

TEST_CASE("preview: the composite is opaque and carries visible panel content") {
    CScene scene;
    buildPreviewScene(scene, 1000);
    SImage img = renderPreview(scene, 800, 500, 768, 384);
    REQUIRE(img.w == 800);
    REQUIRE(img.h == 500);
    REQUIRE(img.rgba.size() == 800u * 500u * 4u);

    // Background is fully opaque everywhere.
    bool allOpaque = true;
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 255) { allOpaque = false; break; }
    CHECK(allOpaque);

    // Panels composited on top brighten many pixels above the dark background floor.
    uint32_t bright = 0;
    for (size_t i = 0; i < img.rgba.size(); i += 4)
        if (img.rgba[i] > 80 || img.rgba[i + 1] > 80 || img.rgba[i + 2] > 80)
            bright++;
    CHECK(bright > 1000u);
}
