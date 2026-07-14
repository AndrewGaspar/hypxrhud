#pragma once

#include "PanelText.hpp"

#include <string>

// hypxrhud offline evidence path. Writes an SImage to a PNG so panels are reviewable
// without a headset (the --preview composite). Premultiplied source alpha is
// un-premultiplied for a correct on-disk PNG. Uses vendored stb_image_write. Lifted
// from hypxrvoice's PngWrite (src/PngWrite.cpp @ 200a80e).

namespace hud {
namespace Png {
    // Write `img` to `path`. Returns false on I/O / encoder failure.
    bool write(const SImage& img, const std::string& path);
}
} // namespace hud
