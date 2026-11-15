#pragma once
// Heightmap terrain (component "Terrain"): height + splat storage, sampling, ray queries, sculpt / paint
// brushes, LOD tile meshes with skirts, an albedo bake from the 4 splat layers, and foliage scatter.
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"
#include "fledge/scene/components.h"

namespace fledge::world {

enum class BrushMode { Raise, Lower, Smooth, Flatten, Paint };

class Heightfield {
public:
    Heightfield() = default;
    Heightfield(int resolution, float size);
    static Heightfield fromComponent(const TerrainComponent& t);  // decodes heights/splat, or generates from seed
    void toComponent(TerrainComponent& t) const;                  // encodes heights + splat (base64)

    int resolution() const { return m_res; }
    float size() const { return m_size; }
    float spacing() const { return m_size / static_cast<float>(m_res - 1); }
    float& at(int x, int z) { return m_h[static_cast<size_t>(z) * m_res + x]; }
    float at(int x, int z) const { return m_h[static_cast<size_t>(z) * m_res + x]; }
    uint8_t* splatAt(int x, int z) { return &m_splat[(static_cast<size_t>(z) * m_res + x) * 4]; }
    const std::vector<float>& heights() const { return m_h; }

    // Local space: the terrain is centred on its entity, x/z in [-size/2, size/2].
    float heightAt(float x, float z) const;  // bilinear
    Vec3 normalAt(float x, float z) const;
    bool contains(float x, float z) const;
    std::optional<Vec3> raycast(Vec3 origin, Vec3 dir, float maxDistance) const;  // local ray, returns local hit

    void brush(Vec3 center, float radius, float strength, BrushMode mode, int layer = 0, float flattenHeight = 0);
    void generateHills(int seed, float maxHeight);

    int tilesPerEdge(int tileQuads) const { return std::max(1, (m_res - 1 + tileQuads - 1) / tileQuads); }
    // Tile (tx, tz) at LOD l (vertex stride 2^l), with downward skirts that hide LOD cracks.
    MeshData tileMesh(int tx, int tz, int lod, int tileQuads) const;
    // RGBA8 albedo (sRGB) from the splat weights x layer colours, texRes^2.
    std::vector<uint8_t> bakeAlbedo(int texRes, const Vec3 layers[4]) const;

private:
    int m_res = 0;
    float m_size = 0;
    std::vector<float> m_h;
    std::vector<uint8_t> m_splat;
};

// LOD for a tile at `distance` (m): 0 near, up to maxLod far; `previous` adds hysteresis (no popping back
// and forth at a boundary).
int terrainLod(float distance, float tileWorldSize, int previous = -1, int maxLod = 4);

// Deterministic foliage instances (world matrices). terrain/terrainWorld are optional (height snap).
std::vector<Mat4> scatterFoliage(const FoliageComponent& f, const Mat4& world, const Heightfield* terrain = nullptr, const Mat4* terrainWorld = nullptr);

} // namespace fledge::world
