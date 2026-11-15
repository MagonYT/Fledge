// Null RHI backend: accepts every call, allocates handle ids, counts draws.
// Used for headless runs (CI smoke tests, MCP server without a GPU). It
// reports full pipeline caps so the complete render graph executes headless.
#include <unordered_map>

#include "fledge/render/rhi.h"

namespace fledge::rhi {
namespace {

class NullDevice final : public RenderDevice {
public:
    Backend backend() const override { return Backend::Null; }
    std::string description() const override { return "Null RHI (headless)"; }
    DeviceCaps caps() const override {
        DeviceCaps c{true, true, 4};
        c.instancing = true;
        return c;
    }

    MeshHandle createMesh(const MeshData& d) override {
        uint32_t id = m_next++;
        m_triCount[id] = static_cast<uint32_t>(d.indices.size() / 3);
        return {id};
    }
    MeshHandle createLineMesh(std::span<const LineVertex> l) override {
        uint32_t id = m_next++;
        m_lineCount[id] = static_cast<uint32_t>(l.size() / 2);
        return {id};
    }
    void updateLineMesh(MeshHandle m, std::span<const LineVertex> l) override { m_lineCount[m.id] = static_cast<uint32_t>(l.size() / 2); }
    void destroyMesh(MeshHandle m) override {
        m_triCount.erase(m.id);
        m_lineCount.erase(m.id);
    }
    ShaderHandle createShader(const ShaderDesc&) override { return {m_next++}; }
    void destroyShader(ShaderHandle) override {}
    TextureHandle createTexture(const TextureDesc&, std::span<const TextureSubresource>) override { return {m_next++}; }
    void generateMips(TextureHandle) override {}
    bool updateTexture(TextureHandle h, const TextureSubresource&) override { return h.id != 0; }
    TextureHandle createTexture2D(int, int, const uint8_t*) override { return {m_next++}; }
    TextureHandle createTexture(TextureFormat, std::span<const TextureMipDesc>, bool) override { return {m_next++}; }
    bool supportsTextureFormat(TextureFormat) const override { return true; }
    void destroyTexture(TextureHandle) override {}
    uint64_t nativeTexture(TextureHandle) const override { return 0; }
    FramebufferHandle createFramebuffer(const FramebufferDesc&) override { return {m_next++}; }
    void destroyFramebuffer(FramebufferHandle) override {}
    void resolve(FramebufferHandle, FramebufferHandle, int, int) override {}
    RenderTargetHandle createRenderTarget(int, int) override { return {m_next++}; }
    void resizeRenderTarget(RenderTargetHandle, int, int) override {}
    void destroyRenderTarget(RenderTargetHandle) override {}
    uint64_t nativeColorTexture(RenderTargetHandle) const override { return 0; }
    FramebufferHandle framebufferOf(RenderTargetHandle rt) const override { return {rt.id}; }
    BufferHandle createUniformBuffer(size_t) override { return {m_next++}; }
    void updateBuffer(BufferHandle, const void*, size_t, size_t) override {}
    void bindUniformBuffer(int, BufferHandle) override {}
    void destroyBuffer(BufferHandle) override {}
    void beginPass(FramebufferHandle, int, int, int, int, const ClearDesc& c) override {
        if (c.color) m_clear = c.colorValue;
        ++m_stats.passes;
    }
    void beginPass(RenderTargetHandle, int, int, Vec4 c) override {
        m_clear = c;
        ++m_stats.passes;
    }
    void endPass() override {}
    void setShader(ShaderHandle) override {}
    void setUniform(const char*, const Mat4&) override {}
    void setUniform(const char*, Vec3) override {}
    void setUniform(const char*, Vec4) override {}
    void setUniform(const char*, float) override {}
    void setUniform(const char*, int) override {}
    void bindTexture(int, TextureHandle) override {}
    void setDepthTest(bool) override {}
    void setDepthWrite(bool) override {}
    void setDepthFunc(DepthFunc) override {}
    void setBlendMode(BlendMode) override {}
    void setCullMode(CullMode) override {}
    void setWireframe(bool) override {}
    void draw(MeshHandle m, Topology t) override {
        ++m_stats.drawCalls;
        if (t == Topology::Triangles) m_stats.triangles += m_triCount[m.id];
        else m_stats.lines += m_lineCount[m.id];
    }
    void drawInstanced(MeshHandle m, int n) override {
        ++m_stats.drawCalls;
        m_stats.triangles += m_triCount[m.id] * static_cast<uint32_t>(n > 0 ? n : 0);
    }
    void drawFullscreenTriangle() override {
        ++m_stats.drawCalls;
        ++m_stats.triangles;
    }
    bool readPixels(RenderTargetHandle, int w, int h, std::vector<uint8_t>& rgba) override { return fill(w, h, rgba); }
    bool readFramebuffer(FramebufferHandle, int w, int h, std::vector<uint8_t>& rgba) override { return fill(w, h, rgba); }
    FrameStats& stats() override { return m_stats; }

private:
    bool fill(int w, int h, std::vector<uint8_t>& rgba) {
        // No rasterizer: a solid image of the last clear color so callers still get a valid picture.
        rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i] = static_cast<uint8_t>(m_clear.x * 255);
            rgba[i + 1] = static_cast<uint8_t>(m_clear.y * 255);
            rgba[i + 2] = static_cast<uint8_t>(m_clear.z * 255);
            rgba[i + 3] = 255;
        }
        return true;
    }
    uint32_t m_next = 1;
    std::unordered_map<uint32_t, uint32_t> m_triCount, m_lineCount;
    FrameStats m_stats;
    Vec4 m_clear{};
};

} // namespace

std::unique_ptr<RenderDevice> createNullDevice() { return std::make_unique<NullDevice>(); }

} // namespace fledge::rhi
