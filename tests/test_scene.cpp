#include "doctest.h"

#include "Scene.hpp"

using namespace hud;

namespace {
    SUpsert mk(const std::string& owner, const std::string& slot, int urgency = 1) {
        SUpsert u;
        u.owner   = owner;
        u.slot    = slot;
        u.urgency = urgency;
        u.content.lines = {{"panel", EColor::Normal, false}};
        return u;
    }
    bool dismissedHas(const std::vector<SDismissal>& d, uint32_t id, const std::string& reason) {
        for (auto& x : d)
            if (x.id == id && x.reason == reason)
                return true;
        return false;
    }
}

TEST_CASE("scene: layer budget is min(runtime, 16), spec-min on unknown") {
    CHECK(layerBudget(128) == 16); // our stack -> capped at the internal cap
    CHECK(layerBudget(16)  == 16);
    CHECK(layerBudget(8)   == 8);  // a runtime that only guarantees fewer
    CHECK(layerBudget(0)   == 16); // unknown -> spec minimum
    CHECK(layerBudget(-1)  == 16);
}

TEST_CASE("scene: create returns ids; get() finds them") {
    CScene s;
    uint32_t a = s.upsert(mk("x", ""), 1000);
    uint32_t b = s.upsert(mk("x", ""), 1000);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK(s.get(a) != nullptr);
    CHECK(s.panels().size() == 2);
}

TEST_CASE("scene: per-client cap refuses a fifth panel from one owner") {
    CScene s(4);
    for (int i = 0; i < 4; i++)
        CHECK(s.upsert(mk("busy", ""), 1000) != 0);
    CHECK(s.upsert(mk("busy", ""), 1000) == 0); // 5th rejected
    CHECK(s.ownerCount("busy") == 4);
    // A different client is unaffected.
    CHECK(s.upsert(mk("other", ""), 1000) != 0);
}

TEST_CASE("scene: singleton slot — equal urgency is last-writer-wins") {
    CScene s;
    uint32_t first = s.upsert(mk("a", "voice", 1), 1000);
    std::vector<SDismissal> dz;
    uint32_t second = s.upsert(mk("b", "voice", 1), 1100, &dz);
    CHECK(second != 0);
    CHECK(dismissedHas(dz, first, "preempted"));
    CHECK(s.get(first) == nullptr);
    CHECK(s.get(second) != nullptr);
    // Exactly one occupant of the slot.
    int inVoice = 0;
    for (auto& [id, p] : s.panels())
        if (p.slot == "voice") inVoice++;
    CHECK(inVoice == 1);
}

TEST_CASE("scene: singleton slot — a lower-urgency newcomer is refused") {
    CScene s;
    uint32_t occupant = s.upsert(mk("a", "voice", 2), 1000);
    std::vector<SDismissal> dz;
    uint32_t loser = s.upsert(mk("b", "voice", 1), 1100, &dz);
    CHECK(loser == 0);
    CHECK(dz.empty());
    CHECK(s.get(occupant) != nullptr);
}

TEST_CASE("scene: singleton slot — higher urgency preempts") {
    CScene s;
    uint32_t low = s.upsert(mk("a", "voice", 1), 1000);
    std::vector<SDismissal> dz;
    uint32_t crit = s.upsert(mk("b", "voice", 2), 1100, &dz);
    CHECK(crit != 0);
    CHECK(dismissedHas(dz, low, "preempted"));
    CHECK(s.get(low) == nullptr);
}

TEST_CASE("scene: toast slot stacks and evicts the oldest past max") {
    CScene s;
    s.slots().findMut("toast")->maxStack = 3;
    uint32_t t1 = s.upsert(mk("t", "toast"), 1000);
    uint32_t t2 = s.upsert(mk("t", "toast"), 1010);
    uint32_t t3 = s.upsert(mk("t", "toast"), 1020);
    std::vector<SDismissal> dz;
    uint32_t t4 = s.upsert(mk("t", "toast"), 1030, &dz);
    CHECK(t4 != 0);
    CHECK(dismissedHas(dz, t1, "preempted")); // oldest evicted
    CHECK(s.get(t1) == nullptr);
    CHECK(s.get(t2) != nullptr);
    CHECK(s.get(t3) != nullptr);
    CHECK(s.get(t4) != nullptr);
    int inToast = 0;
    for (auto& [id, p] : s.panels())
        if (p.slot == "toast") inToast++;
    CHECK(inToast == 3);
}

TEST_CASE("scene: dropOwner auto-dismisses all of a client's panels (client-gone)") {
    CScene s;
    uint32_t a1 = s.upsert(mk("gone", ""), 1000);
    uint32_t a2 = s.upsert(mk("gone", ""), 1000);
    uint32_t b1 = s.upsert(mk("stays", ""), 1000);
    std::vector<SDismissal> dz;
    s.dropOwner("gone", &dz);
    CHECK(dismissedHas(dz, a1, "client-gone"));
    CHECK(dismissedHas(dz, a2, "client-gone"));
    CHECK(s.get(a1) == nullptr);
    CHECK(s.get(a2) == nullptr);
    CHECK(s.get(b1) != nullptr);
}

TEST_CASE("scene: dismiss reports unknown vs known") {
    CScene s;
    uint32_t a = s.upsert(mk("x", ""), 1000);
    CHECK_FALSE(s.dismiss(9999, "client"));
    CHECK(s.dismiss(a, "client"));
    CHECK(s.get(a) == nullptr);
}

TEST_CASE("scene: upload-on-change — epoch bumps only when content changes") {
    CScene s;
    uint32_t id = s.upsert(mk("v", "voice"), 1000);
    REQUIRE(s.get(id));
    uint64_t e0 = s.get(id)->epoch;

    // Redundant identical update: no epoch bump (a static panel costs zero).
    SUpsert same = mk("v", "voice");
    same.id = id;
    s.upsert(same, 2000);
    CHECK(s.get(id)->epoch == e0);

    // Changed content: epoch bumps + dwell resets.
    SUpsert diff = mk("v", "voice");
    diff.id = id;
    diff.content.lines = {{"open browser", EColor::Normal, false}};
    s.upsert(diff, 3000);
    CHECK(s.get(id)->epoch == e0 + 1);
    CHECK(s.get(id)->shownAtMs == 3000);
}

TEST_CASE("scene: submitOrder respects the layer budget and keeps priority + recency") {
    CScene s(8);
    SUpsert u;
    u.owner = "x";
    u.fade.holdMs = -1; // persistent -> always visible
    u.content.lines = {{"p", EColor::Normal, false}};
    u.urgency = 2; uint32_t hi = s.upsert(u, 1000);           // high urgency, oldest
    u.urgency = 1; s.upsert(u, 1000);                          // id2
    u.urgency = 1; s.upsert(u, 1000);                          // id3
    u.urgency = 1; uint32_t newest = s.upsert(u, 1000);        // id4, newest low-urgency

    auto order = s.submitOrder(2000, /*budget*/ 2);
    REQUIRE(order.size() == 2);
    // The high-urgency panel is always kept; among equals the newest survives.
    bool keepsHi = false, keepsNewest = false;
    for (uint32_t id : order) {
        if (id == hi) keepsHi = true;
        if (id == newest) keepsNewest = true;
    }
    CHECK(keepsHi);
    CHECK(keepsNewest);
    // Submission order is bottom -> top: the last entry is the highest priority.
    CHECK(s.get(order.back())->urgency >= s.get(order.front())->urgency);
}

TEST_CASE("scene: placement resolves slot default, stack offset, and pose override") {
    CScene s;
    // Slot default (voice bottom-centre).
    uint32_t v = s.upsert(mk("a", "voice"), 1000);
    SPlacement pv = s.placementOf(*s.get(v));
    CHECK(pv.px == doctest::Approx(0.0f));
    CHECK(pv.py == doctest::Approx(-0.28f));
    CHECK(pv.space == "view");

    // Free placement with an explicit pose override.
    SUpsert f;
    f.owner = "a";
    f.slot  = "";
    f.hasPose = true; f.px = 0.2f; f.py = 0.3f; f.pz = -2.0f;
    f.content.lines = {{"free", EColor::Normal, false}};
    uint32_t fid = s.upsert(f, 1000);
    SPlacement pf = s.placementOf(*s.get(fid));
    CHECK(pf.px == doctest::Approx(0.2f));
    CHECK(pf.py == doctest::Approx(0.3f));
    CHECK(pf.pz == doctest::Approx(-2.0f));

    // Toast stack: newest at base, older pushed up by one stackDy.
    const SSlot* toast = s.slots().find("toast");
    REQUIRE(toast);
    uint32_t older = s.upsert(mk("t", "toast"), 1000);
    uint32_t newer = s.upsert(mk("t", "toast"), 1010);
    SPlacement po = s.placementOf(*s.get(older));
    SPlacement pn = s.placementOf(*s.get(newer));
    CHECK(pn.py == doctest::Approx(toast->py));                    // newest at base
    CHECK(po.py == doctest::Approx(toast->py + toast->stackDy));   // older pushed up
}

TEST_CASE("scene: reapExpired drops faded transient panels but keeps persistent ones") {
    CScene s;
    SUpsert transient = mk("a", "");
    transient.fade = {10, 20, 10, 0.9f}; // total 40 ms
    uint32_t t = s.upsert(transient, 1000);

    SUpsert persistent = mk("b", "");
    persistent.fade.holdMs = -1;
    uint32_t p = s.upsert(persistent, 1000);

    std::vector<SDismissal> dz;
    s.reapExpired(1000 + 41, &dz); // past the transient's envelope
    CHECK(dismissedHas(dz, t, "expired"));
    CHECK(s.get(t) == nullptr);
    CHECK(s.get(p) != nullptr); // persistent never expires
}
