#pragma once
// AssetLoader: how GameWorld turns an asset path into CPU data.
//   CookedAssetLoader (runtime + default): reads ONLY cooked files
//     ("X.obj" -> "X.obj.cmesh", "Y.png" -> "Y.png.ctex"), no source parsing.
//   The editor installs a source loader (editor/core) that parses OBJ/PNG
//   directly so changes on disk show up immediately.
#include <optional>
#include <string>

#include "fledge/assets/cooked.h"
#include "fledge/core/vfs.h"

namespace fledge {

class AssetLoader {
public:
    virtual ~AssetLoader() = default;
    // vpath = resolved source path, e.g. "content:/Meshes/pyramid.obj"
    virtual std::optional<MeshData> loadMesh(const std::string& vpath, std::string* error) = 0;
    virtual std::optional<TextureData> loadTexture(const std::string& vpath, std::string* error) = 0;
};

class CookedAssetLoader final : public AssetLoader {
public:
    explicit CookedAssetLoader(const Vfs& vfs) : m_vfs(vfs) {}
    std::optional<MeshData> loadMesh(const std::string& vpath, std::string* error) override;
    std::optional<TextureData> loadTexture(const std::string& vpath, std::string* error) override;

private:
    std::optional<Bytes> readCooked(const std::string& vpath, std::string* error);
    const Vfs& m_vfs;
};

} // namespace fledge
