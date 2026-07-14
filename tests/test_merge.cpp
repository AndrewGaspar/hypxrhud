#include "doctest.h"

#include "Props.hpp"
#include "Scene.hpp"

using namespace hud;

// BUG-2: a partial UpdatePanel must MERGE — only the keys the client supplied change; every
// absent field is preserved. These drive the REAL path (upsertFromProps -> CScene::upsert)
// exactly as the D-Bus front end does, so they guard the exact battery-panel regression: an
// update carrying only gauges snapped the panel from its bottom-left slot to centre-FoV.

namespace {
    SConfig defcfg() { return SConfig{}; } // opacity 0.92, hold 2600, rise 110, fade 450.

    std::vector<SGauge> gauges(float pct) {
        return {{"headset", pct, true}, {"laptop", 47.f, false}};
    }
}

TEST_CASE("merge: the battery regression — a gauges-only update keeps the slot placement") {
    CScene s;

    // Battery client CreatePanel: slot=battery, kind=gauges, hold_ms=-1, gauges=[...].
    SPropMap create;
    create["slot"]    = SPropValue::str("battery");
    create["kind"]    = SPropValue::str("gauges");
    create["hold_ms"] = SPropValue::integer(-1);
    create["gauges"]  = SPropValue::gaugeList(gauges(83.f));
    uint32_t id = s.upsert(upsertFromProps(0, ":1.7", create, defcfg()), 1000);
    REQUIRE(id != 0);

    // It spawns on the bottom-left battery slot.
    SPlacement before = s.placementOf(*s.get(id));
    CHECK(before.px == doctest::Approx(-0.55f));
    CHECK(before.py == doctest::Approx(-0.20f));
    CHECK(before.sizeW == doctest::Approx(0.30f));

    // Battery client UpdatePanel (its real payload): kind+hold_ms+gauges, NO slot.
    SPropMap update;
    update["kind"]    = SPropValue::str("gauges");
    update["hold_ms"] = SPropValue::integer(-1);
    update["gauges"]  = SPropValue::gaugeList(gauges(80.f));
    s.upsert(upsertFromProps(id, ":1.7", update, defcfg()), 31000);

    // REGRESSION GUARD: it stays on the battery slot — NOT snapped to centre-FoV (0,0,-1).
    const SPanel* p = s.get(id);
    REQUIRE(p);
    CHECK(p->slot == "battery");
    CHECK_FALSE(p->hasPose);
    SPlacement after = s.placementOf(*p);
    CHECK(after.px == doctest::Approx(-0.55f));
    CHECK(after.py == doctest::Approx(-0.20f));
    CHECK(after.sizeW == doctest::Approx(0.30f));
}

TEST_CASE("merge: a gauges-only update preserves pose, size, opacity, slot, urgency, timing") {
    CScene s;

    SPropMap create;
    create["slot"]    = SPropValue::str("voice");
    create["urgency"] = SPropValue::integer(2);
    create["pose"]    = SPropValue::vec3(0.1f, 0.2f, -1.5f);
    create["size"]    = SPropValue::dbl(0.6);
    create["opacity"] = SPropValue::dbl(0.5);
    create["rise_ms"] = SPropValue::integer(70);
    create["hold_ms"] = SPropValue::integer(-1);
    create["fade_ms"] = SPropValue::integer(300);
    create["gauges"]  = SPropValue::gaugeList(gauges(90.f));
    uint32_t id = s.upsert(upsertFromProps(0, "o", create, defcfg()), 1000);
    REQUIRE(id != 0);

    // Update with ONLY new gauges.
    SPropMap update;
    update["gauges"] = SPropValue::gaugeList(gauges(88.f));
    s.upsert(upsertFromProps(id, "o", update, defcfg()), 2000);

    const SPanel* p = s.get(id);
    REQUIRE(p);
    CHECK(p->slot == "voice");
    CHECK(p->urgency == 2);
    CHECK(p->hasPose);
    CHECK(p->px == doctest::Approx(0.1f));
    CHECK(p->py == doctest::Approx(0.2f));
    CHECK(p->pz == doctest::Approx(-1.5f));
    CHECK(p->hasSize);
    CHECK(p->sizeW == doctest::Approx(0.6f));
    CHECK(p->fade.opacityCeil == doctest::Approx(0.5f));
    CHECK(p->fade.riseMs == 70);
    CHECK(p->fade.holdMs == -1);
    CHECK(p->fade.fadeMs == 300);
    // The gauges DID change.
    REQUIRE(p->content.gauges.size() == 2);
    CHECK(p->content.gauges[0].percent == doctest::Approx(88.f));
}

TEST_CASE("merge: an update of a single field changes only that field") {
    CScene s;

    SPropMap create;
    create["slot"]    = SPropValue::str("voice");
    create["urgency"] = SPropValue::integer(1);
    create["opacity"] = SPropValue::dbl(0.9);
    create["lines"]   = SPropValue::lineList({{"hello", EColor::Normal, false}});
    uint32_t id = s.upsert(upsertFromProps(0, "o", create, defcfg()), 1000);
    REQUIRE(id != 0);
    const uint64_t e0 = s.get(id)->epoch;

    // Only opacity changes.
    SPropMap o;
    o["opacity"] = SPropValue::dbl(0.3);
    s.upsert(upsertFromProps(id, "o", o, defcfg()), 2000);
    {
        const SPanel* p = s.get(id);
        CHECK(p->fade.opacityCeil == doctest::Approx(0.3f));
        CHECK(p->urgency == 1);           // untouched
        CHECK(p->slot == "voice");        // untouched
        REQUIRE(p->content.lines.size() == 1);
        CHECK(p->content.lines[0].text == "hello"); // content untouched
        CHECK(p->epoch == e0);            // no content change -> no re-raster
    }

    // Only urgency changes.
    SPropMap ug;
    ug["urgency"] = SPropValue::integer(2);
    s.upsert(upsertFromProps(id, "o", ug, defcfg()), 3000);
    {
        const SPanel* p = s.get(id);
        CHECK(p->urgency == 2);
        CHECK(p->fade.opacityCeil == doctest::Approx(0.3f)); // still the last value
        CHECK(p->slot == "voice");
    }
}

TEST_CASE("merge: content epoch bumps only when the merged content actually changes") {
    CScene s;

    SPropMap create;
    create["slot"]   = SPropValue::str("battery");
    create["gauges"] = SPropValue::gaugeList(gauges(50.f));
    uint32_t id = s.upsert(upsertFromProps(0, "o", create, defcfg()), 1000);
    const uint64_t e0 = s.get(id)->epoch;

    // Identical gauges -> zero-cost (no epoch bump, dwell not reset).
    SPropMap same;
    same["gauges"] = SPropValue::gaugeList(gauges(50.f));
    s.upsert(upsertFromProps(id, "o", same, defcfg()), 2000);
    CHECK(s.get(id)->epoch == e0);
    CHECK(s.get(id)->shownAtMs == 1000);

    // Changed gauges -> one bump + dwell reset.
    SPropMap diff;
    diff["gauges"] = SPropValue::gaugeList(gauges(51.f));
    s.upsert(upsertFromProps(id, "o", diff, defcfg()), 3000);
    CHECK(s.get(id)->epoch == e0 + 1);
    CHECK(s.get(id)->shownAtMs == 3000);
}

TEST_CASE("merge: a partial update preserves existing lines when only gauges arrive") {
    CScene s;

    SPropMap create;
    create["kind"]  = SPropValue::str("text");
    create["title"] = SPropValue::str("Status");
    create["lines"] = SPropValue::lineList({{"all good", EColor::Good, false}});
    uint32_t id = s.upsert(upsertFromProps(0, "o", create, defcfg()), 1000);
    REQUIRE(s.get(id)->content.lines.size() == 2); // title + line

    // Update carrying only a confidence bar must NOT wipe the lines.
    SPropMap c;
    c["confidence"] = SPropValue::dbl(0.8);
    s.upsert(upsertFromProps(id, "o", c, defcfg()), 2000);
    const SPanel* p = s.get(id);
    CHECK(p->content.confidence == doctest::Approx(0.8f));
    REQUIRE(p->content.lines.size() == 2);
    CHECK(p->content.lines[0].text == "Status");
    CHECK(p->content.lines[1].text == "all good");
}
