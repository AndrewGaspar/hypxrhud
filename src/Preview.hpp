#pragma once

#include "PanelText.hpp"
#include "Scene.hpp"

#include <string>

// hypxrhud offline preview — the review artifact. Builds a demo six-slot scene (voice
// transcript, keys lanes, a toast, a status badge, media now-playing, and the
// dual-gauge battery panel), then composites every panel onto one big canvas by a
// simple pinhole projection of its VIEW-space placement, so a reviewer sees the whole
// HUD layout in a PNG without a headset. This exercises the real Scene + Slots + Panel
// + PanelText path; only the projection-to-2D is preview-specific (in a headset each
// panel is its own head-locked quad).

namespace hud {

// Populate `scene` with the six demo panels (one per slot). Used by --preview and by
// the preview unit test.
void buildPreviewScene(CScene& scene, int64_t nowMs);

// Composite the scene's panels into a W x H image. Each panel is rasterised at
// texW x texH and blitted at its projected screen position and scale, at its peak
// opacity (envelope timing is ignored — the preview shows every panel fully visible).
SImage renderPreview(const CScene& scene, int W, int H, int texW, int texH);

} // namespace hud
