#include "doctest.h"

#include "Presentation.hpp"
#include "Scene.hpp"

#include <algorithm>

using namespace hud;

namespace {
    SUpsert panel(const std::string& owner, const std::string& slot = {}, int urgency = 1) {
        SUpsert value;
        value.owner = owner;
        value.slot = slot;
        value.urgency = urgency;
        value.fade = {0, -1, 0, 0.92f};
        value.content.lines = {{"synthetic", EColor::Normal, false}};
        return value;
    }
}

TEST_CASE("presentation tracker advances only for successful shouldRender frames") {
    CPresentationTracker tracker;
    tracker.recordFrame(false, {7}); // shouldRender=false or failed xrEndFrame.
    CHECK(tracker.snapshot(7).frameSerial == 0);
    CHECK(tracker.snapshot(7).panelSerial == 0);

    tracker.recordFrame(true, {7});
    const auto shown = tracker.snapshot(7);
    CHECK(shown.frameSerial == 1);
    CHECK(shown.panelSerial == shown.frameSerial);

    tracker.recordFrame(true, {}); // a successful frame that omitted the panel.
    const auto omitted = tracker.snapshot(7);
    CHECK(omitted.frameSerial == 2);
    CHECK(omitted.panelSerial == 1);
    CHECK(omitted.panelSerial != omitted.frameSerial);

    tracker.recordFrame(true, {7}); // restored presentation starts a new continuous streak.
    const auto restored = tracker.snapshot(7);
    CHECK(restored.panelSerial == restored.frameSerial);
    CHECK(restored.streakStart == restored.frameSerial);

    tracker.dismiss(7);
    CHECK(tracker.snapshot(7).panelSerial == 0);
}

TEST_CASE("presentation acknowledgement follows submitOrder queue and budget decisions") {
    SUBCASE("budget-dropped panel is not acknowledged") {
        CScene scene;
        const uint32_t older = scene.upsert(panel("older"), 1000);
        const uint32_t newer = scene.upsert(panel("newer"), 1010);
        const auto submitted = scene.submitOrder(1100, 1);
        REQUIRE(submitted.size() == 1);
        CHECK(submitted[0] == newer);

        CPresentationTracker tracker;
        tracker.recordFrame(true, submitted);
        CHECK(tracker.snapshot(newer).panelSerial == tracker.snapshot(newer).frameSerial);
        CHECK(tracker.snapshot(older).panelSerial != tracker.snapshot(older).frameSerial);
    }

    SUBCASE("queued singleton loser is not acknowledged") {
        CScene scene;
        scene.slots().findMut("keys")->onRefuse = ERefusePolicy::Queue;
        const uint32_t occupant = scene.upsert(panel("occupant", "keys", 2), 1000);
        const uint32_t queued = scene.upsert(panel("queued", "keys", 1), 1010);
        REQUIRE(scene.get(queued));
        REQUIRE(scene.get(queued)->queued);
        const auto submitted = scene.submitOrder(1100, 16);
        REQUIRE(std::find(submitted.begin(), submitted.end(), occupant) != submitted.end());
        REQUIRE(std::find(submitted.begin(), submitted.end(), queued) == submitted.end());

        CPresentationTracker tracker;
        tracker.recordFrame(true, submitted);
        CHECK(tracker.snapshot(occupant).panelSerial == tracker.snapshot(occupant).frameSerial);
        CHECK(tracker.snapshot(queued).panelSerial != tracker.snapshot(queued).frameSerial);
    }
}
