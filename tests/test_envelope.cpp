#include "doctest.h"

#include "Panel.hpp"

using namespace hud;

TEST_CASE("envelope: rises, holds, and falls to zero") {
    SFade f;
    f.riseMs = 100; f.holdMs = 1000; f.fadeMs = 200; f.opacityCeil = 0.9f;

    CHECK(envelopeOpacity(f, 0)     == doctest::Approx(0.f));
    CHECK(envelopeOpacity(f, 50)    == doctest::Approx(0.45f));  // mid-rise
    CHECK(envelopeOpacity(f, 100)   == doctest::Approx(0.9f));   // top of rise
    CHECK(envelopeOpacity(f, 600)   == doctest::Approx(0.9f));   // hold plateau
    CHECK(envelopeOpacity(f, 1100)  == doctest::Approx(0.9f));   // end of hold
    CHECK(envelopeOpacity(f, 1200)  == doctest::Approx(0.45f));  // mid-fade
    CHECK(envelopeOpacity(f, 1300)  == doctest::Approx(0.f));    // fully faded
    CHECK(envelopeOpacity(f, 99999) == doctest::Approx(0.f));
}

TEST_CASE("envelope: persistent panel never auto-fades") {
    SFade f;
    f.riseMs = 100; f.holdMs = -1; f.opacityCeil = 0.9f;
    CHECK(envelopeOpacity(f, 100)    == doctest::Approx(0.9f));
    CHECK(envelopeOpacity(f, 100000) == doctest::Approx(0.9f));
}

TEST_CASE("envelope: negative elapsed and zero fade clamp cleanly") {
    SFade f;
    f.riseMs = 100; f.holdMs = 500; f.fadeMs = 0; f.opacityCeil = 0.8f;
    CHECK(envelopeOpacity(f, -10) == doctest::Approx(0.f));
    CHECK(envelopeOpacity(f, 700) == doctest::Approx(0.f)); // zero fade => instant off past hold
}
