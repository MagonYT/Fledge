#pragma once
// CPU image (RGBA8) and PNG encoding. Decoding of source images (PNG/JPG/...)
// lives in fledge_importers (editor/tools only); the runtime only reads cooked
// textures (texture_data.h).
#include <cstdint>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge {

struct ImageData {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool hasAlpha() const;  // any pixel with alpha < 255
};

Bytes encodePng(const ImageData& image);

} // namespace fledge
