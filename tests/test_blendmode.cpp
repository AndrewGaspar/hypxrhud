#include "doctest.h"

#include "BlendMode.hpp"

using namespace hud;

namespace {
    // WiVRn advertises OPAQUE then ALPHA_BLEND (opaque preferred-first). A plain VR runtime
    // advertises OPAQUE only. An optical see-through advertises ADDITIVE.
    const std::vector<EBlendMode> kWiVRn   = {EBlendMode::Opaque, EBlendMode::Alpha};
    const std::vector<EBlendMode> kOpaque  = {EBlendMode::Opaque};
    const std::vector<EBlendMode> kAdd     = {EBlendMode::Additive};
    const std::vector<EBlendMode> kEmpty   = {};
}

TEST_CASE("blend: auto prefers ALPHA for the overlay when the runtime advertises it") {
    // The passthrough bug: on WiVRn an OPAQUE overlay paints the whole view black. "auto"
    // must pick ALPHA_BLEND so the panels composite over passthrough.
    auto pick = pickBlendMode(kWiVRn, "auto");
    CHECK(pick.mode == EBlendMode::Alpha);
    CHECK_FALSE(pick.requestedUnsupported);
}

TEST_CASE("blend: auto falls back to the runtime-preferred mode when ALPHA is absent") {
    auto p1 = pickBlendMode(kOpaque, "auto");
    CHECK(p1.mode == EBlendMode::Opaque);
    CHECK_FALSE(p1.requestedUnsupported);

    auto p2 = pickBlendMode(kAdd, "auto"); // additive-only display -> take its preferred
    CHECK(p2.mode == EBlendMode::Additive);
    CHECK_FALSE(p2.requestedUnsupported);
}

TEST_CASE("blend: an unrecognized config value behaves like auto") {
    CHECK(pickBlendMode(kWiVRn, "").mode == EBlendMode::Alpha);
    CHECK(pickBlendMode(kWiVRn, "banana").mode == EBlendMode::Alpha);
}

TEST_CASE("blend: an explicit advertised choice is honored exactly") {
    CHECK(pickBlendMode(kWiVRn, "opaque").mode == EBlendMode::Opaque);
    CHECK_FALSE(pickBlendMode(kWiVRn, "opaque").requestedUnsupported);
    CHECK(pickBlendMode(kWiVRn, "alpha").mode == EBlendMode::Alpha);
    CHECK_FALSE(pickBlendMode(kWiVRn, "alpha").requestedUnsupported);
}

TEST_CASE("blend: an explicit UNsupported choice falls back to preferred + flags it") {
    // alpha requested but the runtime only offers opaque -> fall back, flag for a warning.
    auto pick = pickBlendMode(kOpaque, "alpha");
    CHECK(pick.mode == EBlendMode::Opaque); // preferred (first-listed)
    CHECK(pick.requestedUnsupported);

    // additive requested on WiVRn (opaque, alpha) -> fall back to preferred (opaque), flagged.
    auto p2 = pickBlendMode(kWiVRn, "additive");
    CHECK(p2.mode == EBlendMode::Opaque);
    CHECK(p2.requestedUnsupported);
}

TEST_CASE("blend: an empty advertised list (spec-illegal) defends to OPAQUE") {
    CHECK(pickBlendMode(kEmpty, "auto").mode == EBlendMode::Opaque);
    CHECK(pickBlendMode(kEmpty, "alpha").mode == EBlendMode::Opaque);
    CHECK(pickBlendMode(kEmpty, "alpha").requestedUnsupported); // explicit want, none advertised
    CHECK(pickBlendMode(kEmpty, "opaque").mode == EBlendMode::Opaque);
}

TEST_CASE("blend: blendModeName round-trips the enum") {
    CHECK(std::string(blendModeName(EBlendMode::Opaque)) == "opaque");
    CHECK(std::string(blendModeName(EBlendMode::Alpha)) == "alpha");
    CHECK(std::string(blendModeName(EBlendMode::Additive)) == "additive");
}
