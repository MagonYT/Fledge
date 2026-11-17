#include "fledge/assets/package.h"

namespace fledge {

GamePackage openGamePackage(const fs::path& file) {
    GamePackage pkg;
    // v2 first: new archives are the common case going forward.
    if (auto r = FpackReader::openAppended(file)) {
        pkg.format = PakFormat::V2;
        pkg.v2 = std::move(r);
        return pkg;
    }
    if (auto r = FpackReader::openFile(file)) {
        pkg.format = PakFormat::V2;
        pkg.v2 = std::move(r);
        return pkg;
    }
    if (auto r = PakReader::openAppended(file)) {
        pkg.format = PakFormat::V1;
        pkg.v1 = std::move(r);
        return pkg;
    }
    if (auto r = PakReader::openFile(file)) {
        pkg.format = PakFormat::V1;
        pkg.v1 = std::move(r);
        return pkg;
    }
    return pkg;
}

std::shared_ptr<FileProvider> providerFor(const GamePackage& pkg) {
    if (pkg.format == PakFormat::V2) return std::make_shared<FpackProvider>(pkg.v2);
    if (pkg.format == PakFormat::V1) return std::make_shared<PakProvider>(pkg.v1);
    return nullptr;
}

} // namespace fledge
