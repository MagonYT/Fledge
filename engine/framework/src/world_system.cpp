#include "fledge/framework/world_system.h"

namespace fledge {

namespace {
std::vector<WorldSystemFactory>& factories() {
    static std::vector<WorldSystemFactory> f;
    return f;
}
} // namespace

void registerWorldSystem(WorldSystemFactory factory) {
    for (auto& f : factories())
        if (f.name == factory.name) {
            f = std::move(factory);
            return;
        }
    factories().push_back(std::move(factory));
}

const std::vector<WorldSystemFactory>& worldSystemFactories() { return factories(); }

} // namespace fledge
