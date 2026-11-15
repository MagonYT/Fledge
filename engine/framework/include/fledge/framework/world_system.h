#pragma once
// World systems of linked modules (1.3.0, docs/ADVANCED_GAMEPLAY.md "Update order").
//
// An Advanced module (engine/advanced/*) registers a factory at edition start-up (initEdition); every GameWorld then creates
// one instance per factory when Play begins and destroys it when Play ends. Fledge (Standard) links no module that
// registers one, so the list is empty there and GameWorld::tick does exactly what it did before.
//
// Order inside GameWorld::tick (while playing):
//   modules -> scripts (fixed + OnUpdate) -> built-in gameplay -> prePhysics(dt) -> physics steps, each preceded by
//   fixedStep(h) -> animation -> postAnimation(dt) -> scripts OnLateUpdate -> late gameplay -> streaming -> audio
// fixedStep runs only when the physics world exists (the scene has colliders / characters); prePhysics runs every frame.
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fledge {

class GameWorld;

class WorldSystem {
public:
    virtual ~WorldSystem() = default;
    virtual void beginPlay(GameWorld&) {}
    virtual void endPlay(GameWorld&) {}
    virtual void prePhysics(GameWorld&, float /*dt*/) {}
    virtual void fixedStep(GameWorld&, float /*h*/) {}
    virtual void postAnimation(GameWorld&, float /*dt*/) {}
};

struct WorldSystemFactory {
    std::string name;  // "advanced.weapons" (GameWorld::system(name) finds the live instance)
    std::function<std::unique_ptr<WorldSystem>()> create;
};

// Process-wide list (idempotent by name). Called by module register functions only.
void registerWorldSystem(WorldSystemFactory factory);
const std::vector<WorldSystemFactory>& worldSystemFactories();

} // namespace fledge
