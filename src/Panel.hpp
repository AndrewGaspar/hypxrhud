#pragma once

#include <cstdint>
#include <string>
#include <vector>

// hypxrhud — the PURE panel content model. Nothing here touches OpenXR, EGL, GL, or a
// font: it is the "what to show" description a client hands the daemon, generalised out
// of hypxrvoice's SHudView (src/HudModel.hpp @ 200a80e) so multiple XR utilities
// (voice, keys, toasts, media, battery) share one vocabulary. The rasteriser
// (PanelText) turns it into pixels and the overlay session (Session) submits one quad
// per panel. Keeping this layer pure is what makes the daemon reviewable offline
// (--preview) and unit-testable.
//
// This struct is deliberately the shape of the eventual D-Bus `a{sv}` props (design
// memo §2.2): lines+colorRole, a confidence bar, named gauges, fade envelope. The
// interim stdin NDJSON transport (Wire) and the future D-Bus front end (WP-H3) both
// build an SPanelContent — H3 replaces the transport, not the model.

namespace hud {

// A semantic colour role; PanelText maps each to concrete RGBA. Kept abstract so the
// palette lives in one place (the theming seam, WP-H6) and the model stays
// render-agnostic. Values match hypxrvoice's EHudColor so the migration is mechanical.
enum class EColor {
    Normal, // primary foreground.
    Dim,    // secondary / hint text.
    Accent, // the verb / focus.
    Good,   // success / high confidence / charging.
    Warn,   // caution / low confidence / low battery.
    Bad,    // error / critical battery.
};
const char* colorName(EColor c);
EColor      colorFromName(const std::string& s); // inverse; unknown -> Normal.

struct SLine {
    std::string text;
    EColor      color = EColor::Normal;
    bool        big   = false; // render larger (the title line).
};

// A named gauge (battery-slot content): a labelled percentage with a charging flag.
// The battery panel carries TWO — headset battery (WiVRn) and laptop battery (upower) —
// so the content model holds a list. Reusable for any future metered widget.
struct SGauge {
    std::string label;          // "headset", "laptop", …
    float       percent  = -1.f; // [0,100]; <0 = unknown (drawn as "—").
    bool        charging = false;
};

// What KIND of content a panel carries. Text is the common case (voice/keys/toast/
// status/media); Gauges is the battery-style multi-gauge readout.
enum class EPanelKind {
    Text,
    Gauges,
};
const char* panelKindName(EPanelKind k);
EPanelKind  panelKindFromName(const std::string& s);

// A fully-resolved panel body. The renderer stamps its own monotonic clock when the
// panel is shown, so the model never needs the compositor clock.
struct SPanelContent {
    EPanelKind          kind = EPanelKind::Text;
    std::vector<SLine>  lines;   // Text panels (and the title/label rows generally).
    std::vector<SGauge> gauges;  // Gauges panels (battery).
    float               confidence = -1.f; // [0,1] draws a certainty bar; <0 = no bar.

    bool empty() const {
        return (kind == EPanelKind::Text && lines.empty()) ||
               (kind == EPanelKind::Gauges && gauges.empty());
    }
};

// Fade envelope (renderer-clock ms). holdMs < 0 => persist until the panel is updated
// or dismissed (the voice "listening" case). Rise+hold+fade for transient panels.
// Lifted verbatim from hypxrvoice's SHudView timing fields.
struct SFade {
    int   riseMs      = 110;
    int   holdMs      = 2600;
    int   fadeMs      = 450;
    float opacityCeil = 0.92f;
};

// Layer opacity for a panel shown `elapsedMs` ago, following the rise/hold/fade
// envelope. Pure — the overlay session calls this every frame for a free,
// texture-upload-free fade (color-scale-bias .a). Returns [0, opacityCeil].
// (This is hypxrvoice's hudOpacity, generalised off the timing struct.)
float envelopeOpacity(const SFade& f, int64_t elapsedMs);

} // namespace hud
