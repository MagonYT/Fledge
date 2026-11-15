#include "fledge/assets/image.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fledge {

bool ImageData::hasAlpha() const {
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] != 255) return true;
    return false;
}

Bytes encodePng(const ImageData& image) {
    Bytes out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* o = static_cast<Bytes*>(ctx);
            auto* d = static_cast<uint8_t*>(data);
            o->insert(o->end(), d, d + size);
        },
        &out, image.width, image.height, 4, image.rgba.data(), image.width * 4);
    return out;
}

} // namespace fledge
