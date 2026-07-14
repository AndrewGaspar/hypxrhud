#pragma once

#include "Panel.hpp"

#include <cstdint>
#include <vector>

// hypxrhud panel rasteriser. Turns an SPanelContent (pure model) into an RGBA image
// with PREMULTIPLIED alpha, top-row-first, ready either to be written as a PNG
// (--preview) or uploaded verbatim into a swapchain image. Uses the vendored
// stb_truetype with a bundled OFL font (third_party/fonts/LiberationMono) baked into
// the binary — no runtime font-path dependency, no system font libs. CPU only; no
// OpenXR/EGL/GL here, so it lives in the shared core and is exercised offline.
//
// Lifted from hypxrvoice's HudText (src/HudText.cpp @ 200a80e) and generalised: the
// text/confidence path is the voice HUD unchanged; the Gauges path is new (the battery
// panel's dual headset+laptop readout). The EColor -> RGBA palette is the single
// theming seam (WP-H6 will source it from the Omarchy theme).

namespace hud {

struct SImage {
    int                  w = 0;
    int                  h = 0;
    std::vector<uint8_t> rgba; // w*h*4, premultiplied, top row first.
    bool                 empty() const { return rgba.empty(); }
};

// Render `content` into a texW x texH transparent canvas with a compact rounded panel
// hugging the content (transparent margins let the world/monitors show through). An
// empty content yields a fully transparent image. Deterministic — same content in,
// same pixels out (the offline correctness surface).
SImage renderPanel(const SPanelContent& content, int texW = 768, int texH = 384);

} // namespace hud
