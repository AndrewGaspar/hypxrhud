#include "Panel.hpp"

#include <algorithm>

namespace hud {

const char* colorName(EColor c) {
    switch (c) {
        case EColor::Normal: return "normal";
        case EColor::Dim:    return "dim";
        case EColor::Accent: return "accent";
        case EColor::Good:   return "good";
        case EColor::Warn:   return "warn";
        case EColor::Bad:    return "bad";
    }
    return "normal";
}

EColor colorFromName(const std::string& s) {
    if (s == "dim")    return EColor::Dim;
    if (s == "accent") return EColor::Accent;
    if (s == "good")   return EColor::Good;
    if (s == "warn")   return EColor::Warn;
    if (s == "bad")    return EColor::Bad;
    return EColor::Normal;
}

const char* panelKindName(EPanelKind k) {
    switch (k) {
        case EPanelKind::Text:   return "text";
        case EPanelKind::Gauges: return "gauges";
    }
    return "text";
}

EPanelKind panelKindFromName(const std::string& s) {
    if (s == "gauges")
        return EPanelKind::Gauges;
    return EPanelKind::Text;
}

// Rise -> hold -> fade envelope. holdMs < 0 => never auto-fades (persistent panel).
// Byte-for-byte the behaviour of hypxrvoice's hudOpacity (src/HudModel.cpp:16).
float envelopeOpacity(const SFade& f, int64_t elapsedMs) {
    const float ceil = std::clamp(f.opacityCeil, 0.f, 1.f);
    if (elapsedMs < 0)
        return 0.f;
    if (f.riseMs > 0 && elapsedMs < f.riseMs)
        return ceil * (static_cast<float>(elapsedMs) / f.riseMs);
    const int64_t afterRise = elapsedMs - std::max(0, f.riseMs);
    if (f.holdMs < 0)
        return ceil; // persistent — no auto-fade.
    if (afterRise < f.holdMs)
        return ceil;
    const int64_t intoFade = afterRise - f.holdMs;
    if (f.fadeMs <= 0)
        return 0.f;
    if (intoFade < f.fadeMs)
        return ceil * (1.f - static_cast<float>(intoFade) / f.fadeMs);
    return 0.f;
}

} // namespace hud
