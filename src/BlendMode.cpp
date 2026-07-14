#include "BlendMode.hpp"

namespace hud {

const char* blendModeName(EBlendMode m) {
    switch (m) {
        case EBlendMode::Opaque:   return "opaque";
        case EBlendMode::Alpha:    return "alpha";
        case EBlendMode::Additive: return "additive";
    }
    return "opaque";
}

SBlendPick pickBlendMode(const std::vector<EBlendMode>& supported, const std::string& config) {
    // The runtime's preferred mode = its first-listed one (empty list = spec-noncompliant
    // runtime; fall back to OPAQUE so we never submit an unadvertised mode).
    const EBlendMode preferred = supported.empty() ? EBlendMode::Opaque : supported.front();

    auto advertises = [&](EBlendMode m) {
        for (auto s : supported)
            if (s == m)
                return true;
        return false;
    };

    // Explicit request: honor iff advertised, else fall back to preferred with a flag.
    if (config == "opaque" || config == "alpha" || config == "additive") {
        const EBlendMode want = config == "opaque"   ? EBlendMode::Opaque
                                : config == "alpha"  ? EBlendMode::Alpha
                                                     : EBlendMode::Additive;
        if (advertises(want))
            return {want, false};
        return {preferred, true};
    }

    // "auto" (or anything unrecognized): an overlay HUD prefers ALPHA so its panels composite
    // over the runtime's passthrough underlay; otherwise take the runtime's preferred mode.
    if (advertises(EBlendMode::Alpha))
        return {EBlendMode::Alpha, false};
    return {preferred, false};
}

} // namespace hud
