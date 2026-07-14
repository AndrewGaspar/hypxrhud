#include "doctest.h"

#include "Scene.hpp"

#include <algorithm>

using namespace hud;

// WP-H5 — slot arbitration polish: on_refuse=queue held-pending panels, promotion on slot
// free, and per-slot occupancy introspection. (The refuse/preempt/last-writer/stack policy
// itself is covered in test_scene.cpp.)

namespace {
    // A persistent panel (rise 0, hold forever) so submitOrder visibility is timing-independent.
    SUpsert mk(const std::string& owner, const std::string& slot, int urgency) {
        SUpsert u;
        u.owner   = owner;
        u.slot    = slot;
        u.urgency = urgency;
        u.fade    = {0, -1, 0, 0.92f};
        u.content.lines = {{"panel", EColor::Normal, false}};
        return u;
    }
    bool contains(const std::vector<uint32_t>& v, uint32_t id) {
        return std::find(v.begin(), v.end(), id) != v.end();
    }
    const SSlotStat* stat(const std::vector<SSlotStat>& v, const std::string& name) {
        for (const auto& s : v)
            if (s.name == name)
                return &s;
        return nullptr;
    }
}

TEST_CASE("arbiter: on_refuse=queue holds a lower-urgency loser pending, not refused") {
    CScene s;
    s.slots().findMut("voice")->onRefuse = ERefusePolicy::Queue;

    uint32_t occ  = s.upsert(mk("a", "voice", 2), 1000);           // urgency-2 occupant
    std::vector<SDismissal> dz;
    uint32_t wait = s.upsert(mk("b", "voice", 1), 1010, &dz);      // urgency-1 loser -> queued

    CHECK(wait != 0);          // accepted (not the hard-refusal 0).
    CHECK(dz.empty());         // the occupant was NOT preempted.
    REQUIRE(s.get(occ));
    REQUIRE(s.get(wait));
    CHECK_FALSE(s.get(occ)->queued);
    CHECK(s.get(wait)->queued);

    // The queued panel is not submitted for rendering; the occupant is.
    auto order = s.submitOrder(2000, 16);
    CHECK(contains(order, occ));
    CHECK_FALSE(contains(order, wait));

    // Occupancy introspection reflects one active + one queued in `voice`.
    const SSlotStat* st = stat(s.slotStats(), "voice");
    REQUIRE(st);
    CHECK(st->active == 1);
    CHECK(st->queued == 1);
}

TEST_CASE("arbiter: dismissing the occupant promotes the queued panel into the slot") {
    CScene s;
    s.slots().findMut("voice")->onRefuse = ERefusePolicy::Queue;
    uint32_t occ  = s.upsert(mk("a", "voice", 2), 1000);
    uint32_t wait = s.upsert(mk("b", "voice", 1), 1010);
    REQUIRE(s.get(wait)->queued);

    s.dismiss(occ, "client", 2000); // nowMs triggers promotion.
    CHECK(s.get(occ) == nullptr);
    REQUIRE(s.get(wait));
    CHECK_FALSE(s.get(wait)->queued);          // promoted -> active.
    CHECK(s.get(wait)->shownAtMs == 2000);     // envelope restarted at promotion.
    CHECK(contains(s.submitOrder(3000, 16), wait));
}

TEST_CASE("arbiter: promotion picks highest urgency, then oldest") {
    CScene s;
    s.slots().findMut("voice")->onRefuse = ERefusePolicy::Queue;
    uint32_t occ   = s.upsert(mk("a", "voice", 2), 1000);
    uint32_t lowA  = s.upsert(mk("b", "voice", 0), 1010); // queued, urgency 0 (older)
    uint32_t midB  = s.upsert(mk("c", "voice", 1), 1020); // queued, urgency 1
    uint32_t lowC  = s.upsert(mk("d", "voice", 0), 1030); // queued, urgency 0 (newer)
    REQUIRE(s.get(lowA)->queued);
    REQUIRE(s.get(midB)->queued);
    REQUIRE(s.get(lowC)->queued);

    s.dismiss(occ, "client", 2000);
    // Highest urgency (midB) wins the slot; the two urgency-0 panels stay queued.
    CHECK_FALSE(s.get(midB)->queued);
    CHECK(s.get(lowA)->queued);
    CHECK(s.get(lowC)->queued);

    // Now dismiss the promoted one — the OLDER of the two equal-urgency queued (lowA) wins.
    s.dismiss(midB, "client", 2100);
    CHECK_FALSE(s.get(lowA)->queued);
    CHECK(s.get(lowC)->queued);
}

TEST_CASE("arbiter: an expiring occupant promotes a queued panel") {
    CScene s;
    s.slots().findMut("voice")->onRefuse = ERefusePolicy::Queue;

    SUpsert occ = mk("a", "voice", 2);
    occ.fade    = {0, 20, 0, 0.9f}; // transient: total 20 ms
    uint32_t occId  = s.upsert(occ, 1000);
    uint32_t waitId = s.upsert(mk("b", "voice", 1), 1005);
    REQUIRE(s.get(waitId)->queued);

    std::vector<SDismissal> dz;
    s.reapExpired(1000 + 21, &dz); // occupant's envelope elapsed
    CHECK(s.get(occId) == nullptr);
    REQUIRE(s.get(waitId));
    CHECK_FALSE(s.get(waitId)->queued); // promoted when the slot freed.
}

TEST_CASE("arbiter: dropOwner of the occupant promotes another client's queued panel") {
    CScene s;
    s.slots().findMut("voice")->onRefuse = ERefusePolicy::Queue;
    uint32_t occ  = s.upsert(mk("owner-a", "voice", 2), 1000);
    uint32_t wait = s.upsert(mk("owner-b", "voice", 1), 1010);
    REQUIRE(s.get(wait)->queued);

    std::vector<SDismissal> dz;
    s.dropOwner("owner-a", &dz, 2000);
    CHECK(s.get(occ) == nullptr);
    REQUIRE(s.get(wait));
    CHECK_FALSE(s.get(wait)->queued);
}

TEST_CASE("arbiter: default (refuse) policy is unchanged — the loser is rejected") {
    CScene s; // voice defaults to ERefusePolicy::Refuse.
    uint32_t occ = s.upsert(mk("a", "voice", 2), 1000);
    std::vector<SDismissal> dz;
    uint32_t loser = s.upsert(mk("b", "voice", 1), 1010, &dz);
    CHECK(loser == 0);
    CHECK(dz.empty());
    REQUIRE(s.get(occ));
    const SSlotStat* st = stat(s.slotStats(), "voice");
    REQUIRE(st);
    CHECK(st->active == 1);
    CHECK(st->queued == 0);
}

TEST_CASE("arbiter: stack slots never queue (a toast just stacks)") {
    CScene s;
    // Even if a stack slot were somehow marked queue, stacking takes precedence.
    s.slots().findMut("toast")->onRefuse = ERefusePolicy::Queue;
    uint32_t t1 = s.upsert(mk("t", "toast", 2), 1000);
    uint32_t t2 = s.upsert(mk("t", "toast", 1), 1010); // lower urgency, but stacks — not queued
    REQUIRE(s.get(t1));
    REQUIRE(s.get(t2));
    CHECK_FALSE(s.get(t1)->queued);
    CHECK_FALSE(s.get(t2)->queued);
    const SSlotStat* st = stat(s.slotStats(), "toast");
    REQUIRE(st);
    CHECK(st->active == 2);
    CHECK(st->queued == 0);
}

TEST_CASE("arbiter: slotStats lists every slot in registry order") {
    CScene s;
    auto stats = s.slotStats();
    REQUIRE(stats.size() == s.slots().all().size());
    for (const char* n : {"voice", "keys", "toast", "status", "media", "battery"})
        CHECK(stat(stats, n) != nullptr);
}
