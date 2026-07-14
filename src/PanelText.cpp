#include "PanelText.hpp"

#include "stb_truetype.h" // implementation in stb_impl.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// The bundled OFL font, baked into the binary by CMake (font_data.c). See
// third_party/fonts/LiberationMono-OFL.txt for the licence.
extern "C" const unsigned char g_hudFontData[];
extern "C" const unsigned int  g_hudFontDataSize;

namespace hud {

namespace {
    // WP-H6: colours now come from an SPalette (Theme.hpp), threaded into renderPanel, so a
    // themed daemon and --preview --theme render in the active Omarchy palette. SRgb lives in
    // Theme.hpp; the concrete role->colour lookup is paletteColor().

    struct SFont {
        stbtt_fontinfo info{};
        bool           ok = false;
        SFont() {
            ok = stbtt_InitFont(&info, g_hudFontData,
                                stbtt_GetFontOffsetForIndex(g_hudFontData, 0)) != 0;
        }
    };
    const SFont& font() {
        static SFont f;
        return f;
    }

    uint32_t nextCodepoint(const std::string& s, size_t& i) {
        auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) { i += 1; return b0; }
        auto cont = [&](size_t k) {
            return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
        };
        if ((b0 & 0xE0) == 0xC0 && cont(i + 1)) {
            uint32_t cp = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            i += 2; return cp;
        }
        if ((b0 & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
            uint32_t cp = ((b0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            i += 3; return cp;
        }
        if ((b0 & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            uint32_t cp = ((b0 & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            i += 4; return cp;
        }
        i += 1;
        return 0xFFFD;
    }

    struct SCanvas {
        int                  w, h;
        std::vector<uint8_t> px; // premultiplied, top-row-first.
        SCanvas(int W, int H) : w(W), h(H), px(static_cast<size_t>(W) * H * 4, 0) {}

        void blend(int x, int y, float sr, float sg, float sb, float sa) {
            if (x < 0 || y < 0 || x >= w || y >= h || sa <= 0.f)
                return;
            uint8_t* d   = &px[(static_cast<size_t>(y) * w + x) * 4];
            float    ida = 1.f - sa;
            d[0] = static_cast<uint8_t>(std::min(255.f, sr * 255.f + d[0] * ida));
            d[1] = static_cast<uint8_t>(std::min(255.f, sg * 255.f + d[1] * ida));
            d[2] = static_cast<uint8_t>(std::min(255.f, sb * 255.f + d[2] * ida));
            d[3] = static_cast<uint8_t>(std::min(255.f, sa * 255.f + d[3] * ida));
        }
    };

    void fillRoundRect(SCanvas& c, int x0, int y0, int x1, int y1, int rad, float r, float g,
                       float b, float a) {
        rad = std::max(0, std::min(rad, std::min((x1 - x0) / 2, (y1 - y0) / 2)));
        for (int y = std::max(0, y0); y < std::min(c.h, y1); y++) {
            for (int x = std::max(0, x0); x < std::min(c.w, x1); x++) {
                float cov = 1.f;
                int cx = -1, cy = -1;
                if (x < x0 + rad && y < y0 + rad) { cx = x0 + rad; cy = y0 + rad; }
                else if (x >= x1 - rad && y < y0 + rad) { cx = x1 - rad - 1; cy = y0 + rad; }
                else if (x < x0 + rad && y >= y1 - rad) { cx = x0 + rad; cy = y1 - rad - 1; }
                else if (x >= x1 - rad && y >= y1 - rad) { cx = x1 - rad - 1; cy = y1 - rad - 1; }
                if (cx >= 0) {
                    float d = std::sqrt(static_cast<float>((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                    cov = std::clamp(rad - d + 0.5f, 0.f, 1.f);
                }
                float sa = a * cov;
                c.blend(x, y, r * sa, g * sa, b * sa, sa);
            }
        }
    }

    struct SLineMetrics {
        float scale;
        int   ascent, descent, lineGap;
        int   width;
    };

    SLineMetrics measure(const std::string& text, float pixelHeight) {
        SLineMetrics m{};
        const auto&  f = font();
        m.scale        = stbtt_ScaleForPixelHeight(&f.info, pixelHeight);
        int a, d, g;
        stbtt_GetFontVMetrics(&f.info, &a, &d, &g);
        m.ascent  = static_cast<int>(std::round(a * m.scale));
        m.descent = static_cast<int>(std::round(d * m.scale));
        m.lineGap = static_cast<int>(std::round(g * m.scale));
        float xadv = 0.f;
        for (size_t i = 0; i < text.size();) {
            uint32_t cp = nextCodepoint(text, i);
            int      aw, lsb;
            stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(cp), &aw, &lsb);
            xadv += aw * m.scale;
        }
        m.width = static_cast<int>(std::ceil(xadv));
        return m;
    }

    void drawText(SCanvas& c, const std::string& text, int penX, int penY, float pixelHeight,
                  SRgb color) {
        const auto& f = font();
        float       scale = stbtt_ScaleForPixelHeight(&f.info, pixelHeight);
        float       x     = static_cast<float>(penX);
        const float cr = color.r / 255.f, cg = color.g / 255.f, cb = color.b / 255.f;
        for (size_t i = 0; i < text.size();) {
            uint32_t cp = nextCodepoint(text, i);
            int      aw, lsb;
            stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(cp), &aw, &lsb);

            int gw = 0, gh = 0, xoff = 0, yoff = 0;
            unsigned char* bmp = stbtt_GetCodepointBitmap(&f.info, 0, scale, static_cast<int>(cp),
                                                          &gw, &gh, &xoff, &yoff);
            if (bmp) {
                int gx0 = static_cast<int>(std::round(x)) + xoff;
                int gy0 = penY + yoff;
                for (int gy = 0; gy < gh; gy++)
                    for (int gx = 0; gx < gw; gx++) {
                        float cov = bmp[gy * gw + gx] / 255.f;
                        if (cov <= 0.f)
                            continue;
                        c.blend(gx0 + gx, gy0 + gy, cr * cov, cg * cov, cb * cov, cov);
                    }
                stbtt_FreeBitmap(bmp, nullptr);
            }
            x += aw * scale;
        }
    }

    // Colour a battery level: full/charging green, low amber, critical red.
    EColor gaugeColor(const SGauge& g) {
        if (g.percent < 0.f)         return EColor::Dim;
        if (g.charging)              return EColor::Good;
        if (g.percent <= 15.f)       return EColor::Bad;
        if (g.percent <= 35.f)       return EColor::Warn;
        return EColor::Good;
    }

    std::string gaugePercentText(const SGauge& g) {
        if (g.percent < 0.f)
            return "—";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%%s", static_cast<int>(std::round(g.percent)),
                      g.charging ? " +" : "");
        return buf;
    }
}

SImage renderPanel(const SPanelContent& content, int texW, int texH, const SPalette& pal) {
    SImage img;
    img.w = texW;
    img.h = texH;
    img.rgba.assign(static_cast<size_t>(texW) * texH * 4, 0);

    if (content.empty() || !font().ok)
        return img; // fully transparent.

    SCanvas c(texW, texH);

    const float titlePx = std::round(texH * 0.135f);
    const float bodyPx  = std::round(texH * 0.095f);
    const int   padX    = static_cast<int>(std::round(texW * 0.045f));
    const int   padY    = static_cast<int>(std::round(texH * 0.06f));
    const int   lineGap = static_cast<int>(std::round(bodyPx * 0.35f));

    // ---- measure to size the panel ----
    struct SLaid {
        const SLine* line;
        float        px;
        SLineMetrics m;
    };
    std::vector<SLaid> laid;
    int contentH = 0, contentW = 0;
    for (const auto& ln : content.lines) {
        float px = ln.big ? titlePx : bodyPx;
        SLineMetrics m = measure(ln.text, px);
        laid.push_back({&ln, px, m});
        contentH += (m.ascent - m.descent) + lineGap;
        contentW = std::max(contentW, m.width);
    }

    // Confidence bar occupies one extra body row.
    const bool showBar = content.confidence >= 0.f;
    const int  barH    = static_cast<int>(std::round(bodyPx * 0.28f));
    if (showBar)
        contentH += barH + lineGap;

    // Gauges: a label baseline + a bar beneath it. The row must clear the label's full
    // ascent/descent plus the bar, or successive rows overlap.
    const int gaugeRowH = static_cast<int>(std::round(bodyPx * 1.75f));
    if (!content.gauges.empty()) {
        contentW = std::max(contentW, static_cast<int>(std::round(texW * 0.62f)));
        for (const auto& g : content.gauges) {
            SLineMetrics lm = measure(g.label, bodyPx);
            contentW = std::max(contentW, lm.width + static_cast<int>(std::round(texW * 0.20f)));
            contentH += gaugeRowH + lineGap;
        }
    }

    if (contentH > 0)
        contentH -= lineGap; // no trailing gap.

    int panelW  = std::min(texW - 8, contentW + 2 * padX);
    int panelH  = std::min(texH - 8, contentH + 2 * padY);
    int panelX0 = (texW - panelW) / 2;
    int panelY0 = (texH - panelH) / 2;

    fillRoundRect(c, panelX0, panelY0, panelX0 + panelW, panelY0 + panelH,
                  static_cast<int>(std::round(texH * 0.05f)), pal.panelBg.r / 255.f,
                  pal.panelBg.g / 255.f, pal.panelBg.b / 255.f, pal.panelAlpha);

    // ---- text lines + confidence bar (voice/keys/toast/status/media) ----
    int penY = panelY0 + padY;
    for (auto& l : laid) {
        penY += l.m.ascent;
        drawText(c, l.line->text, panelX0 + padX, penY, l.px, paletteColor(pal, l.line->color));
        penY += (-l.m.descent) + lineGap;

        if (showBar && !content.lines.empty() && l.line == &content.lines.front()) {
            int  barX0 = panelX0 + padX;
            int  barX1 = panelX0 + panelW - padX;
            int  barY0 = penY;
            int  barY1 = penY + barH;
            fillRoundRect(c, barX0, barY0, barX1, barY1, barH / 2, pal.barTrack.r / 255.f,
                          pal.barTrack.g / 255.f, pal.barTrack.b / 255.f, 0.55f);
            int  fillW = static_cast<int>(std::round((barX1 - barX0) * std::clamp(content.confidence, 0.f, 1.f)));
            SRgb bc2 = content.confidence >= 0.75f ? paletteColor(pal, EColor::Good)
                       : content.confidence >= 0.5f ? paletteColor(pal, EColor::Warn)
                                                    : paletteColor(pal, EColor::Bad);
            fillRoundRect(c, barX0, barY0, barX0 + std::max(barH, fillW), barY1, barH / 2,
                          bc2.r / 255.f, bc2.g / 255.f, bc2.b / 255.f, 0.95f);
            penY += barH + lineGap;
        }
    }

    // ---- gauges (battery: headset + laptop) ----
    if (!content.gauges.empty()) {
        const int barH2 = static_cast<int>(std::round(bodyPx * 0.34f));
        for (const auto& g : content.gauges) {
            SRgb        fg  = paletteColor(pal, gaugeColor(g));
            std::string pct = gaugePercentText(g);
            SLineMetrics pm = measure(pct, bodyPx);

            // Label baseline near the top of the row.
            SLineMetrics lm = measure(g.label, bodyPx);
            int labelBaseline = penY + lm.ascent;
            drawText(c, g.label, panelX0 + padX, labelBaseline, bodyPx, paletteColor(pal, EColor::Normal));
            // Percent text right-aligned on the same baseline.
            drawText(c, pct, panelX0 + panelW - padX - pm.width, labelBaseline, bodyPx, fg);

            // Bar under the label spanning the content width.
            int barX0 = panelX0 + padX;
            int barX1 = panelX0 + panelW - padX;
            int barY0 = labelBaseline + (-lm.descent) + static_cast<int>(std::round(bodyPx * 0.12f));
            int barY1 = barY0 + barH2;
            fillRoundRect(c, barX0, barY0, barX1, barY1, barH2 / 2, pal.barTrack.r / 255.f,
                          pal.barTrack.g / 255.f, pal.barTrack.b / 255.f, 0.55f);
            float frac  = g.percent < 0.f ? 0.f : std::clamp(g.percent / 100.f, 0.f, 1.f);
            int   fillW = static_cast<int>(std::round((barX1 - barX0) * frac));
            if (fillW > 0)
                fillRoundRect(c, barX0, barY0, barX0 + std::max(barH2, fillW), barY1, barH2 / 2,
                              fg.r / 255.f, fg.g / 255.f, fg.b / 255.f, 0.95f);
            penY += gaugeRowH + lineGap;
        }
    }

    img.rgba = std::move(c.px);
    return img;
}

} // namespace hud
