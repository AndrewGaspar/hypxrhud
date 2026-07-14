#include "Preview.hpp"

#include <algorithm>
#include <cmath>

namespace hud {

namespace {
    // Pinhole focal length in pixels. Chosen so a ~0.4 m panel at ~1.1 m reads at a
    // comfortable on-canvas size.
    constexpr float kFocal = 900.f;

    SUpsert textPanel(const std::string& slot, std::vector<SLine> lines, float confidence = -1.f,
                      int urgency = 1) {
        SUpsert u;
        u.owner        = "preview." + slot; // each slot models a distinct client (per-client cap).
        u.slot         = slot;
        u.urgency      = urgency;
        u.content.kind = EPanelKind::Text;
        u.content.lines = std::move(lines);
        u.content.confidence = confidence;
        return u;
    }
}

void buildPreviewScene(CScene& scene, int64_t nowMs) {
    // voice — bottom-centre: a live transcript with a confidence bar.
    scene.upsert(textPanel("voice",
                           {{"listening", EColor::Accent, true},
                            {"open the browser", EColor::Normal, false}},
                           0.82f),
                 nowMs);

    // keys — above voice: a screenkey-style key lane sample.
    scene.upsert(textPanel("keys",
                           {{"Super + Return", EColor::Accent, true},
                            {"g i t   c o m m i t", EColor::Normal, false}}),
                 nowMs);

    // toast — top-centre: a mirrored notification.
    scene.upsert(textPanel("toast",
                           {{"Build finished", EColor::Good, true},
                            {"hypxrhud • just now", EColor::Dim, false}}),
                 nowMs);

    // status — bottom-right: a pinned badge.
    scene.upsert(textPanel("status",
                           {{"REC", EColor::Bad, true},
                            {"12:04", EColor::Dim, false}}),
                 nowMs);

    // media — top-left: now-playing.
    scene.upsert(textPanel("media",
                           {{"Now Playing", EColor::Dim, false},
                            {"Aphex Twin", EColor::Accent, true},
                            {"Rhubarb  2:14 / 6:10", EColor::Normal, false}}),
                 nowMs);

    // battery — bottom-left: the DUAL gauge (headset + laptop).
    {
        SUpsert u;
        u.owner        = "preview.battery";
        u.slot         = "battery";
        u.content.kind = EPanelKind::Gauges;
        u.content.lines = {{"battery", EColor::Dim, false}};
        u.content.gauges = {
            {"headset", 83.f, true},  // WiVRn: charging
            {"laptop",  47.f, false}, // upower: discharging
        };
        scene.upsert(u, nowMs);
    }
}

SImage renderPreview(const CScene& scene, int W, int H, int texW, int texH, const SPalette& pal) {
    SImage out;
    out.w = W;
    out.h = H;
    out.rgba.assign(static_cast<size_t>(W) * H * 4, 0);

    // Background: a subtle dark vertical gradient (stand-in for passthrough/space), so
    // the transparent panel margins read against something. Opaque (alpha 255).
    for (int y = 0; y < H; y++) {
        float t = static_cast<float>(y) / std::max(1, H - 1);
        uint8_t r = static_cast<uint8_t>(14 + 10 * (1.f - t));
        uint8_t g = static_cast<uint8_t>(16 + 12 * (1.f - t));
        uint8_t b = static_cast<uint8_t>(22 + 18 * (1.f - t));
        for (int x = 0; x < W; x++) {
            uint8_t* d = &out.rgba[(static_cast<size_t>(y) * W + x) * 4];
            d[0] = r; d[1] = g; d[2] = b; d[3] = 255;
        }
    }

    const float cx = W * 0.5f, cy = H * 0.5f;

    // Composite (premultiplied source over opaque dest), bilinear-sampled scale.
    auto blit = [&](const SImage& src, float dx0, float dy0, float dw, float dh, float mul) {
        int ix0 = static_cast<int>(std::floor(dx0));
        int iy0 = static_cast<int>(std::floor(dy0));
        int ix1 = static_cast<int>(std::ceil(dx0 + dw));
        int iy1 = static_cast<int>(std::ceil(dy0 + dh));
        for (int y = std::max(0, iy0); y < std::min(H, iy1); y++) {
            for (int x = std::max(0, ix0); x < std::min(W, ix1); x++) {
                float u = (x + 0.5f - dx0) / dw;
                float v = (y + 0.5f - dy0) / dh;
                if (u < 0 || u >= 1 || v < 0 || v >= 1)
                    continue;
                float fx = u * src.w - 0.5f, fy = v * src.h - 0.5f;
                int   x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src.w - 1);
                int   y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src.h - 1);
                int   x1 = std::min(x0 + 1, src.w - 1);
                int   y1 = std::min(y0 + 1, src.h - 1);
                float ax = std::clamp(fx - x0, 0.f, 1.f), ay = std::clamp(fy - y0, 0.f, 1.f);
                auto  sample = [&](int k) {
                    auto p = [&](int sx, int sy) {
                        return static_cast<float>(src.rgba[(static_cast<size_t>(sy) * src.w + sx) * 4 + k]);
                    };
                    float top = p(x0, y0) * (1 - ax) + p(x1, y0) * ax;
                    float bot = p(x0, y1) * (1 - ax) + p(x1, y1) * ax;
                    return top * (1 - ay) + bot * ay;
                };
                float sr = sample(0) * mul, sg = sample(1) * mul, sb = sample(2) * mul, sa = sample(3) * mul;
                uint8_t* d = &out.rgba[(static_cast<size_t>(y) * W + x) * 4];
                float    ida = 1.f - sa / 255.f;
                d[0] = static_cast<uint8_t>(std::min(255.f, sr + d[0] * ida));
                d[1] = static_cast<uint8_t>(std::min(255.f, sg + d[1] * ida));
                d[2] = static_cast<uint8_t>(std::min(255.f, sb + d[2] * ida));
                d[3] = 255;
            }
        }
    };

    for (const auto& [id, p] : scene.panels()) {
        SImage     tex   = renderPanel(p.content, texW, texH, pal);
        if (tex.empty())
            continue;
        SPlacement place = scene.placementOf(p);
        float      z     = std::max(0.05f, -place.pz);
        float      pw    = place.sizeW / z * kFocal;                 // projected texture width, px.
        float      ph    = pw * static_cast<float>(texH) / texW;     // keep texture aspect.
        float      sx    = cx + place.px / z * kFocal;
        float      sy    = cy - place.py / z * kFocal;
        blit(tex, sx - pw * 0.5f, sy - ph * 0.5f, pw, ph, std::clamp(p.fade.opacityCeil, 0.f, 1.f));
    }

    // A caption strip so the artifact is self-describing.
    SPanelContent cap;
    cap.lines = {{"hypxrhud — six-slot HUD preview", EColor::Dim, false}};
    SImage capImg = renderPanel(cap, 640, 96, pal);
    blit(capImg, cx - 320.f, 24.f, 640.f, 96.f, 0.9f);

    return out;
}

} // namespace hud
