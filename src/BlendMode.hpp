#pragma once

#include <string>
#include <vector>

// hypxrhud — PURE environment-blend-mode selection. A HAVE_OPENXR-free mirror of
// XrEnvironmentBlendMode so the pick logic is an unconditionally-compiled, unit-testable
// function (no runtime, no openxr headers in hud_core). Session.cpp converts to/from the
// real XrEnvironmentBlendMode. Mirrors HypXRland's OpenXR::pickBlendMode semantics, with
// one deliberate difference: this is an OVERLAY HUD, so "auto" prefers ALPHA (composite the
// panels over the runtime's passthrough underlay) rather than the runtime's first-listed
// mode — an OPAQUE overlay would paint the whole view black over passthrough.

namespace hud {

enum class EBlendMode { Opaque, Alpha, Additive };

// "opaque" | "alpha" | "additive" (the config string form). Unknown -> "opaque".
const char* blendModeName(EBlendMode m);

// Result of pickBlendMode: the chosen mode plus whether an EXPLICIT config request could not
// be honored (the runtime doesn't advertise it), so the caller can emit a one-time warning.
struct SBlendPick {
    EBlendMode mode                 = EBlendMode::Opaque;
    bool       requestedUnsupported = false;
};

// Pure blend-mode selection. `supported` is the runtime's advertised list in preference
// order (xrEnumerateEnvironmentBlendModes returns preferred-first). `config` is [hud]
// blend_mode:
//   "auto" (default; or anything unrecognized) => ALPHA if the runtime advertises it (an
//     overlay HUD wants its panels to alpha-composite over passthrough), else the runtime's
//     first-listed (preferred) mode. No fallback warning.
//   explicit "opaque"|"alpha"|"additive" => honored iff advertised, else fall back to the
//     runtime's preferred mode with requestedUnsupported=true.
// An empty supported list (spec-illegal, but defended) yields Opaque.
SBlendPick pickBlendMode(const std::vector<EBlendMode>& supported, const std::string& config);

} // namespace hud
