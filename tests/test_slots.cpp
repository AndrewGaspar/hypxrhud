#include "doctest.h"

#include "Slots.hpp"

using namespace hud;

TEST_CASE("slots: the six locked defaults are all present") {
    CSlots s;
    for (const char* name : {"voice", "keys", "toast", "status", "media", "battery"})
        CHECK(s.find(name) != nullptr);
    CHECK(s.all().size() == 6);
    CHECK(s.find("nonexistent") == nullptr);
}

TEST_CASE("slots: only the toast slot stacks; the rest are singletons") {
    CSlots s;
    CHECK(s.find("toast")->stack);
    for (const char* name : {"voice", "keys", "status", "media", "battery"})
        CHECK_FALSE(s.find(name)->stack);
}

TEST_CASE("slots: default poses put the corners in the right quadrants") {
    CSlots s;
    // media top-left: x<0, y>0 ; battery bottom-left: x<0, y<0 ; status bottom-right: x>0, y<0.
    CHECK(s.find("media")->px < 0.f);
    CHECK(s.find("media")->py > 0.f);
    CHECK(s.find("battery")->px < 0.f);
    CHECK(s.find("battery")->py < 0.f);
    CHECK(s.find("status")->px > 0.f);
    CHECK(s.find("status")->py < 0.f);
    // voice below keys (both centred).
    CHECK(s.find("voice")->px == doctest::Approx(0.f));
    CHECK(s.find("voice")->py < s.find("keys")->py);
}
