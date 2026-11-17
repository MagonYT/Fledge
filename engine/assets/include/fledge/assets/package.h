#pragma once
// Format-agnostic game package reader. Opens a .fpak/.exe or directory and
// dispatches on the payload magic:
//   * v1 "CAGEPAK1" -> PakReader   (pak.h, monolithic layout)
//   * v2 "CAGEPAK2" -> FpackReader (fpack.h, content-addressed chunks)
// Runtime and tools should go through this so old and new packages stay
// readable without call-site switches.
#include <memory>

#include "fledge/assets/fpack.h"
#include "fledge/assets/pak.h"
#include "fledge/core/vfs.h"

namespace fledge {

enum class PakFormat { None, V1, V2 };

struct GamePackage {
    PakFormat format = PakFormat::None;
    std::shared_ptr<PakReader> v1;    // when format == V1
    std::shared_ptr<FpackReader> v2;  // when format == V2
    explicit operator bool() const { return format != PakFormat::None; }
};

// Opens a package file (or an exe with an appended pak). format==None when
// nothing readable was found.
GamePackage openGamePackage(const fs::path& file);

// Mounts an open package as a VFS provider ("content:/..."). nullptr on
// format==None. The GamePackage keeps ownership shared, so the caller may
// drop its copy after mounting.
std::shared_ptr<FileProvider> providerFor(const GamePackage& pkg);

} // namespace fledge
