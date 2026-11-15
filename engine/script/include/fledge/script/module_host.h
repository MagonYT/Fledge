#pragma once
// Loads gameplay modules (C-ABI DLLs) and drives them against a Scene.
#include <memory>
#include <string>
#include <vector>

#include "fledge/core/utility.h"
#include "fledge/scene/scene.h"
#include "fledge/script/fledge_module_api.h"

namespace fledge {

class ModuleHost {
public:
    ModuleHost();
    ~ModuleHost();
    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    void addSearchPath(fs::path dir) { m_searchPaths.push_back(std::move(dir)); }
    // Loads "<name>.dll" (or lib<name>.dll) from the search paths.
    Status load(const std::string& moduleName);
    void unloadAll();
    std::vector<std::string> loadedModules() const;

    void beginPlay(Scene& scene);
    void tick(Scene& scene, float dt);
    void endPlay(Scene& scene);

private:
    struct Loaded {
        std::string name;
        void* handle = nullptr;
        const FledgeModuleApi* api = nullptr;
    };
    std::vector<fs::path> m_searchPaths;
    std::vector<Loaded> m_modules;
    FledgeHostApi m_hostApi{};
    Scene* m_scene = nullptr;
    double m_time = 0;
    friend struct ModuleHostCallbacks;
};

} // namespace fledge
