#include "PngWrite.hpp"

#include "stb_image_write.h" // implementation in stb_impl.cpp

#include <algorithm>
#include <vector>

namespace hud {
namespace Png {
    bool write(const SImage& img, const std::string& path) {
        if (img.empty() || img.w <= 0 || img.h <= 0)
            return false;

        // stb writes straight (non-premultiplied) RGBA; our canvas is premultiplied, so
        // divide RGB by A.
        std::vector<uint8_t> straight(img.rgba.size());
        for (size_t i = 0; i < img.rgba.size(); i += 4) {
            uint8_t a = img.rgba[i + 3];
            if (a == 0) {
                straight[i] = straight[i + 1] = straight[i + 2] = 0;
                straight[i + 3]                                 = 0;
                continue;
            }
            for (int k = 0; k < 3; k++) {
                int v           = img.rgba[i + k] * 255 / a;
                straight[i + k] = static_cast<uint8_t>(std::min(255, v));
            }
            straight[i + 3] = a;
        }

        return stbi_write_png(path.c_str(), img.w, img.h, 4, straight.data(), img.w * 4) != 0;
    }
}
} // namespace hud
