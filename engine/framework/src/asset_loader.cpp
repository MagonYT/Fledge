#include "fledge/framework/asset_loader.h"

namespace fledge {

std::optional<Bytes> CookedAssetLoader::readCooked(const std::string& vpath, std::string* error) {
    std::string scheme, rel;
    if (!Vfs::split(vpath, scheme, rel)) {
        if (error) *error = "bad asset path " + vpath;
        return std::nullopt;
    }
    std::string cooked = cookedPathFor(rel);
    if (cooked.empty()) {
        if (error) *error = "not a cookable asset: " + vpath;
        return std::nullopt;
    }
    auto bytes = m_vfs.read(scheme + ":/" + cooked);
    if (!bytes && error) *error = "cooked asset missing: " + scheme + ":/" + cooked + " (run fledgepack --cook)";
    return bytes;
}

std::optional<MeshData> CookedAssetLoader::loadMesh(const std::string& vpath, std::string* error) {
    auto b = readCooked(vpath, error);
    return b ? readCookedMesh(*b, error) : std::nullopt;
}

std::optional<TextureData> CookedAssetLoader::loadTexture(const std::string& vpath, std::string* error) {
    auto b = readCooked(vpath, error);
    return b ? readCookedTexture(*b, error) : std::nullopt;
}

} // namespace fledge
