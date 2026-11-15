// OpenGL 3.3 core RHI backend.
#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fledge/core/log.h"
#include "fledge/render/rhi.h"
#include "gl_loader.h"

namespace fledge::rhi {
namespace {

using namespace fledge::gl;

struct GlMesh {
    GLuint vao = 0, vbo = 0, ibo = 0, tbo = 0;
    GLsizei count = 0;  // index count (triangles) or vertex count (lines)
    bool indexed = true;
    uint32_t primitives = 0;
};
struct GlTexture {
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    TextureDesc desc;
};
struct GlTarget {  // legacy RGBA8 render target
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;
    uint32_t fbHandle = 0;
};

struct FormatInfo {
    GLenum internal, format, type;
};
FormatInfo glFormat(TextureFormat f, bool srgb) {
    switch (f) {
        case TextureFormat::RGBA8: return {srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case TextureFormat::RGBA16F: return {GL_RGBA16F, GL_RGBA, GL_FLOAT};
        case TextureFormat::RG16F: return {GL_RG16F, GL_RG, GL_FLOAT};
        case TextureFormat::R16F: return {GL_R16F, GL_RED, GL_FLOAT};
        case TextureFormat::RGBA32F: return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
        case TextureFormat::Depth32F: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
        case TextureFormat::BC1: return {srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_RGBA, GL_UNSIGNED_BYTE};
        case TextureFormat::BC3: return {srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_RGBA, GL_UNSIGNED_BYTE};
    }
    return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
}

class GlDevice final : public RenderDevice {
public:
    GlDevice() {
        auto str = [](GLenum e) { auto* s = glGetString(e); return s ? reinterpret_cast<const char*>(s) : "?"; };
        m_desc = std::string("OpenGL ") + str(GL_VERSION) + " / " + str(GL_RENDERER) + " / " + str(GL_VENDOR);
        FLEDGE_LOG_INFO("rhi", "{}", m_desc);
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        bool s3tc = false, srgb = false;
        for (GLint i = 0; i < n; ++i) {
            const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (!e) continue;
            std::string_view ext(e);
            if (ext == "GL_EXT_texture_compression_s3tc") s3tc = true;
            if (ext == "GL_EXT_texture_sRGB") srgb = true;
        }
        m_s3tc = s3tc && srgb;
        GLint samples = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &samples);
        m_maxSamples = std::max(1, static_cast<int>(samples));
        FLEDGE_LOG_INFO("rhi", "S3TC (BC1/BC3) sRGB textures: {}, max MSAA samples: {}", m_s3tc ? "supported" : "NOT supported (CPU fallback)",
                      m_maxSamples);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        glVertexAttrib4f(3, 1, 0, 0, 1);  // default tangent when a mesh has none
        glGenVertexArrays(1, &m_emptyVao);
    }
    ~GlDevice() override {
        for (auto& [id, m] : m_meshes) freeMesh(m);
        for (auto& [id, p] : m_programs) glDeleteProgram(p);
        for (auto& [id, t] : m_textures) glDeleteTextures(1, &t.id);
        for (auto& [id, t] : m_targets) freeTarget(t);
        for (auto& [id, f] : m_fbos) glDeleteFramebuffers(1, &f);
        for (auto& [id, b] : m_buffers) glDeleteBuffers(1, &b);
        glDeleteVertexArrays(1, &m_emptyVao);
    }

    Backend backend() const override { return Backend::OpenGL; }
    std::string description() const override { return m_desc; }
    DeviceCaps caps() const override {
        DeviceCaps c{true, m_s3tc, m_maxSamples};
        c.instancing = true;
        return c;
    }

    // ------------------------------------------------------------ meshes ---
    MeshHandle createMesh(const MeshData& d) override {
        GlMesh m;
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);
        glGenBuffers(1, &m.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.vertices.size() * sizeof(Vertex)), d.vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &m.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.indices.size() * sizeof(uint32_t)), d.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE_, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE_, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        if (d.tangents.size() == d.vertices.size() && !d.tangents.empty()) {
            glGenBuffers(1, &m.tbo);
            glBindBuffer(GL_ARRAY_BUFFER, m.tbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.tangents.size() * sizeof(Vec4)), d.tangents.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE_, sizeof(Vec4), nullptr);
        }
        glBindVertexArray(0);
        m.count = static_cast<GLsizei>(d.indices.size());
        m.primitives = static_cast<uint32_t>(d.indices.size() / 3);
        uint32_t id = m_next++;
        m_meshes[id] = m;
        return {id};
    }

    MeshHandle createLineMesh(std::span<const LineVertex> lines) override {
        GlMesh m;
        m.indexed = false;
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);
        glGenBuffers(1, &m.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, sizeof(LineVertex), reinterpret_cast<void*>(offsetof(LineVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE_, sizeof(LineVertex), reinterpret_cast<void*>(offsetof(LineVertex, color)));
        glBindVertexArray(0);
        uint32_t id = m_next++;
        m_meshes[id] = m;
        updateLineMesh({id}, lines);
        return {id};
    }

    void updateLineMesh(MeshHandle h, std::span<const LineVertex> lines) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        glBindBuffer(GL_ARRAY_BUFFER, it->second.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lines.size_bytes()), lines.data(), GL_DYNAMIC_DRAW);
        it->second.count = static_cast<GLsizei>(lines.size());
        it->second.primitives = static_cast<uint32_t>(lines.size() / 2);
    }

    void destroyMesh(MeshHandle h) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        freeMesh(it->second);
        m_meshes.erase(it);
    }

    // ----------------------------------------------------------- shaders ---
    ShaderHandle createShader(const ShaderDesc& desc) override {
        GLuint vs = compile(GL_VERTEX_SHADER, desc.glslVertex, desc.name);
        GLuint fs = compile(GL_FRAGMENT_SHADER, desc.glslFragment, desc.name);
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            return {};
        }
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(p, sizeof(log), nullptr, log);
            FLEDGE_LOG_ERROR("rhi", "Link failed for shader '{}': {}", desc.name, log);
            glDeleteProgram(p);
            return {};
        }
        for (auto& [block, binding] : desc.uniformBlocks) {
            GLuint idx = glGetUniformBlockIndex(p, block.c_str());
            if (idx != GL_INVALID_INDEX) glUniformBlockBinding(p, idx, static_cast<GLuint>(binding));
        }
        glUseProgram(p);
        for (auto& [sampler, unit] : desc.samplers) glUniform1i(glGetUniformLocation(p, sampler.c_str()), unit);
        glUseProgram(m_current);
        uint32_t id = m_next++;
        m_programs[id] = p;
        return {id};
    }
    void destroyShader(ShaderHandle h) override {
        if (auto it = m_programs.find(h.id); it != m_programs.end()) {
            m_uniformLocations.erase(it->second);
            glDeleteProgram(it->second);
            m_programs.erase(it);
        }
    }

    // ---------------------------------------------------------- textures ---
    TextureHandle createTexture(const TextureDesc& d, std::span<const TextureSubresource> initial) override {
        if ((d.format == TextureFormat::BC1 || d.format == TextureFormat::BC3) && !m_s3tc) return {};
        GlTexture t;
        t.desc = d;
        t.target = d.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE
                 : d.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP
                 : d.type == TextureType::Tex2DArray ? GL_TEXTURE_2D_ARRAY
                                                     : GL_TEXTURE_2D;
        glGenTextures(1, &t.id);
        glBindTexture(t.target, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        FormatInfo fi = glFormat(d.format, d.srgb);
        if (d.samples > 1) {
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, std::min(d.samples, m_maxSamples), fi.internal, d.width, d.height, GL_TRUE_);
        } else {
            auto findData = [&](int mip, int layer) -> const TextureSubresource* {
                for (auto& s : initial)
                    if (s.mip == mip && s.layer == layer) return &s;
                return nullptr;
            };
            bool compressed = d.format == TextureFormat::BC1 || d.format == TextureFormat::BC3;
            for (int mip = 0; mip < d.mips; ++mip) {
                int w = std::max(1, d.width >> mip), h = std::max(1, d.height >> mip);
                if (d.type == TextureType::Tex2DArray) {
                    glTexImage3D(GL_TEXTURE_2D_ARRAY, mip, static_cast<GLint>(fi.internal), w, h, d.layers, 0, fi.format, fi.type, nullptr);
                } else {
                    int faces = d.type == TextureType::Cube ? 6 : 1;
                    for (int f = 0; f < faces; ++f) {
                        GLenum target = d.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(f) : GL_TEXTURE_2D;
                        const TextureSubresource* src = findData(mip, f);
                        if (compressed) {
                            if (src) glCompressedTexImage2D(target, mip, fi.internal, w, h, 0, static_cast<GLsizei>(src->size), src->data);
                        } else {
                            glTexImage2D(target, mip, static_cast<GLint>(fi.internal), w, h, 0, fi.format, fi.type, src ? src->data : nullptr);
                        }
                    }
                }
            }
            glTexParameteri(t.target, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(t.target, GL_TEXTURE_MAX_LEVEL, d.mips - 1);
            applySampler(t.target, d.sampler, d.mips > 1);
        }
        if (GLenum err = glGetError(); err != 0) {
            FLEDGE_LOG_ERROR("rhi", "createTexture failed (GL error 0x{:x}, format {})", err, toString(d.format));
            glDeleteTextures(1, &t.id);
            return {};
        }
        uint32_t id = m_next++;
        m_textures[id] = t;
        return {id};
    }

    bool updateTexture(TextureHandle h, const TextureSubresource& mip0) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end()) return false;
        GlTexture& t = it->second;
        if (t.target != GL_TEXTURE_2D || t.desc.format != TextureFormat::RGBA8 || t.desc.samples > 1) return false;
        if (mip0.width != t.desc.width || mip0.height != t.desc.height || !mip0.data) return false;
        if (mip0.size < static_cast<size_t>(mip0.width) * static_cast<size_t>(mip0.height) * 4) return false;
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        FormatInfo fi = glFormat(t.desc.format, t.desc.srgb);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mip0.width, mip0.height, fi.format, fi.type, mip0.data);
        return true;
    }

    void generateMips(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end() || it->second.desc.mips <= 1) return;
        glBindTexture(it->second.target, it->second.id);
        glGenerateMipmap(it->second.target);
    }

    TextureHandle createTexture2D(int w, int h, const uint8_t* rgba) override {
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.srgb = true;
        int mips = 1;
        while ((std::max(w, h) >> mips) > 0) ++mips;
        d.mips = mips;
        d.sampler = {Filter::Linear, true, Wrap::Repeat, false};
        TextureSubresource s{0, 0, w, h, rgba, static_cast<size_t>(w) * static_cast<size_t>(h) * 4};
        TextureHandle t = createTexture(d, std::span(&s, 1));
        generateMips(t);
        return t;
    }

    TextureHandle createTexture(TextureFormat format, std::span<const TextureMipDesc> mips, bool srgb) override {
        if (mips.empty()) return {};
        TextureDesc d;
        d.format = format;
        d.srgb = srgb;
        d.width = mips[0].width;
        d.height = mips[0].height;
        d.mips = static_cast<int>(mips.size());
        d.sampler = {Filter::Linear, mips.size() > 1, Wrap::Repeat, false};
        std::vector<TextureSubresource> subs;
        for (size_t i = 0; i < mips.size(); ++i) subs.push_back({static_cast<int>(i), 0, mips[i].width, mips[i].height, mips[i].data, mips[i].size});
        return createTexture(d, subs);
    }
    bool supportsTextureFormat(TextureFormat f) const override {
        return (f != TextureFormat::BC1 && f != TextureFormat::BC3) || m_s3tc;
    }

    void destroyTexture(TextureHandle h) override {
        if (auto it = m_textures.find(h.id); it != m_textures.end()) {
            glDeleteTextures(1, &it->second.id);
            m_textures.erase(it);
        }
    }
    uint64_t nativeTexture(TextureHandle h) const override {
        auto it = m_textures.find(h.id);
        return it == m_textures.end() ? 0 : it->second.id;
    }

    // ------------------------------------------------------ framebuffers ---
    FramebufferHandle createFramebuffer(const FramebufferDesc& d) override {
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        auto attach = [&](GLenum point, const Attachment& a) {
            auto it = m_textures.find(a.texture.id);
            if (it == m_textures.end()) return;
            const GlTexture& t = it->second;
            if (t.target == GL_TEXTURE_2D_ARRAY) glFramebufferTextureLayer(GL_FRAMEBUFFER, point, t.id, a.mip, a.layer);
            else if (t.target == GL_TEXTURE_CUBE_MAP)
                glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(a.layer), t.id, a.mip);
            else glFramebufferTexture2D(GL_FRAMEBUFFER, point, t.target, t.id, a.mip);
        };
        std::array<GLenum, 4> bufs{};
        for (int i = 0; i < d.colorCount; ++i) {
            attach(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), d.colors[static_cast<size_t>(i)]);
            bufs[static_cast<size_t>(i)] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i);
        }
        if (d.colorCount > 0) glDrawBuffers(d.colorCount, bufs.data());
        else {
            GLenum none = GL_NONE;
            glDrawBuffers(1, &none);
            glReadBuffer(GL_NONE);
        }
        if (d.depth.texture) attach(GL_DEPTH_ATTACHMENT, d.depth);
        bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!complete) {
            FLEDGE_LOG_ERROR("rhi", "framebuffer incomplete");
            glDeleteFramebuffers(1, &fbo);
            return {};
        }
        uint32_t id = m_next++;
        m_fbos[id] = fbo;
        auto& att = m_fbAttachments[id];
        for (int i = 0; i < d.colorCount; ++i) att.push_back(d.colors[static_cast<size_t>(i)].texture.id);
        if (d.depth.texture) att.push_back(d.depth.texture.id);
        return {id};
    }
    void destroyFramebuffer(FramebufferHandle h) override {
        if (auto it = m_fbos.find(h.id); it != m_fbos.end()) {
            glDeleteFramebuffers(1, &it->second);
            m_fbos.erase(it);
        }
        m_fbAttachments.erase(h.id);
    }
    GLuint fboOf(FramebufferHandle h) const {
        auto it = m_fbos.find(h.id);
        return it == m_fbos.end() ? 0 : it->second;
    }
    void resolve(FramebufferHandle src, FramebufferHandle dst, int w, int h) override {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboOf(src));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboOf(dst));
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // --------------------------------------------- legacy render targets ---
    RenderTargetHandle createRenderTarget(int w, int h) override {
        uint32_t id = m_next++;
        m_targets[id] = {};
        resizeRenderTarget({id}, w, h);
        return {id};
    }
    void resizeRenderTarget(RenderTargetHandle h, int w, int hgt) override {
        auto it = m_targets.find(h.id);
        if (it == m_targets.end()) return;
        GlTarget& t = it->second;
        if (t.fbo && t.w == w && t.h == hgt) return;
        freeTarget(t);
        t.w = w < 1 ? 1 : w;
        t.h = hgt < 1 ? 1 : hgt;
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glGenTextures(1, &t.color);
        glBindTexture(GL_TEXTURE_2D, t.color);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
        glGenRenderbuffers(1, &t.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t.w, t.h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            FLEDGE_LOG_ERROR("rhi", "Framebuffer incomplete ({}x{})", t.w, t.h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!t.fbHandle) t.fbHandle = m_next++;
        m_fbos[t.fbHandle] = t.fbo;  // aliased: owned by the target
    }
    void destroyRenderTarget(RenderTargetHandle h) override {
        if (auto it = m_targets.find(h.id); it != m_targets.end()) {
            m_fbos.erase(it->second.fbHandle);
            freeTarget(it->second);
            m_targets.erase(it);
        }
    }
    uint64_t nativeColorTexture(RenderTargetHandle h) const override {
        auto it = m_targets.find(h.id);
        return it == m_targets.end() ? 0 : it->second.color;
    }
    FramebufferHandle framebufferOf(RenderTargetHandle h) const override {
        auto it = m_targets.find(h.id);
        return it == m_targets.end() ? FramebufferHandle{} : FramebufferHandle{it->second.fbHandle};
    }

    // ----------------------------------------------------------- buffers ---
    BufferHandle createUniformBuffer(size_t bytes) override {
        GLuint b;
        glGenBuffers(1, &b);
        glBindBuffer(GL_UNIFORM_BUFFER, b);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_DRAW);
        uint32_t id = m_next++;
        m_buffers[id] = b;
        return {id};
    }
    void updateBuffer(BufferHandle h, const void* data, size_t bytes, size_t offset) override {
        auto it = m_buffers.find(h.id);
        if (it == m_buffers.end()) return;
        glBindBuffer(GL_UNIFORM_BUFFER, it->second);
        glBufferSubData(GL_UNIFORM_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
    }
    void bindUniformBuffer(int binding, BufferHandle h) override {
        auto it = m_buffers.find(h.id);
        glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(binding), it == m_buffers.end() ? 0 : it->second);
    }
    void destroyBuffer(BufferHandle h) override {
        if (auto it = m_buffers.find(h.id); it != m_buffers.end()) {
            glDeleteBuffers(1, &it->second);
            m_buffers.erase(it);
        }
    }

    // ------------------------------------------------------------ passes ---
    void beginPass(FramebufferHandle fb, int x, int y, int w, int h, const ClearDesc& c) override {
        // Avoid feedback loops: a texture still bound for sampling (e.g. last frame's shadow map)
        // must not be bound while it is a render target (undefined behaviour in GL).
        if (auto it = m_fbAttachments.find(fb.id); it != m_fbAttachments.end())
            for (uint32_t texId : it->second)
                for (int unit = 0; unit < kMaxUnits; ++unit)
                    if (m_unitTexture[unit] == texId) {
                        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                        glBindTexture(m_unitTarget[unit], 0);
                        m_unitTexture[unit] = 0;
                    }
        glBindFramebuffer(GL_FRAMEBUFFER, fboOf(fb));
        glViewport(x, y, w, h);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDepthMask(GL_TRUE_);
        GLbitfield bits = 0;
        if (c.color) {
            glClearColor(c.colorValue.x, c.colorValue.y, c.colorValue.z, c.colorValue.w);
            bits |= GL_COLOR_BUFFER_BIT;
        }
        if (c.depth) bits |= GL_DEPTH_BUFFER_BIT;
        if (bits) glClear(bits);
        ++m_stats.passes;
    }
    void beginPass(RenderTargetHandle rt, int w, int h, Vec4 c) override {
        ClearDesc cd;
        cd.colorValue = c;
        beginPass(rt ? framebufferOf(rt) : FramebufferHandle{}, 0, 0, w, h, cd);
    }
    void endPass() override { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    // ------------------------------------------------------------- state ---
    void setShader(ShaderHandle h) override {
        auto it = m_programs.find(h.id);
        m_current = it == m_programs.end() ? 0 : it->second;
        glUseProgram(m_current);
    }
    GLint loc(const char* name) {
        if (!m_current || !name) return -1;
        auto& table = m_uniformLocations[m_current];
        if (auto it = table.find(std::string_view(name)); it != table.end()) return it->second;
        GLint location = glGetUniformLocation(m_current, name);
        table.emplace(name, location);
        return location;
    }
    void setUniform(const char* n, const Mat4& m) override { glUniformMatrix4fv(loc(n), 1, GL_FALSE_, m.data()); }
    void setUniform(const char* n, Vec3 v) override { glUniform3fv(loc(n), 1, &v.x); }
    void setUniform(const char* n, Vec4 v) override { glUniform4fv(loc(n), 1, &v.x); }
    void setUniform(const char* n, float f) override { glUniform1f(loc(n), f); }
    void setUniform(const char* n, int i) override { glUniform1i(loc(n), i); }
    void bindTexture(int slot, TextureHandle h) override {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(slot));
        auto it = m_textures.find(h.id);
        if (slot >= 0 && slot < kMaxUnits) {
            m_unitTexture[slot] = it == m_textures.end() ? 0 : h.id;
            m_unitTarget[slot] = it == m_textures.end() ? GL_TEXTURE_2D : it->second.target;
        }
        if (it == m_textures.end()) {
            glBindTexture(GL_TEXTURE_2D, 0);
            return;
        }
        glBindTexture(it->second.target, it->second.id);
    }
    void setDepthTest(bool e) override { e ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST); }
    void setDepthWrite(bool e) override { glDepthMask(e ? GL_TRUE_ : GL_FALSE_); }
    void setDepthFunc(DepthFunc f) override { glDepthFunc(f == DepthFunc::Less ? GL_LESS : f == DepthFunc::Always ? GL_ALWAYS : GL_LEQUAL); }
    void setBlendMode(BlendMode m) override {
        if (m == BlendMode::None) {
            glDisable(GL_BLEND);
            return;
        }
        glEnable(GL_BLEND);
        if (m == BlendMode::Alpha) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        else glBlendFunc(GL_ONE, GL_ONE);
    }
    void setCullMode(CullMode m) override {
        if (m == CullMode::None) {
            glDisable(GL_CULL_FACE);
            return;
        }
        glEnable(GL_CULL_FACE);
        glCullFace(m == CullMode::Front ? GL_FRONT : GL_BACK);
    }
    void setWireframe(bool e) override { glPolygonMode(GL_FRONT_AND_BACK, e ? GL_LINE : GL_FILL); }

    void draw(MeshHandle h, Topology t) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        const GlMesh& m = it->second;
        glBindVertexArray(m.vao);
        if (m.indexed) glDrawElements(t == Topology::Lines ? GL_LINES : GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
        else glDrawArrays(t == Topology::Lines ? GL_LINES : GL_TRIANGLES, 0, m.count);
        glBindVertexArray(0);
        ++m_stats.drawCalls;
        (t == Topology::Lines ? m_stats.lines : m_stats.triangles) += m.primitives;
    }
    void drawInstanced(MeshHandle h, int instances) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end() || !it->second.indexed || instances <= 0) return;
        const GlMesh& m = it->second;
        glBindVertexArray(m.vao);
        glDrawElementsInstanced(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr, instances);
        glBindVertexArray(0);
        ++m_stats.drawCalls;
        m_stats.triangles += m.primitives * static_cast<uint32_t>(instances);
    }
    void drawFullscreenTriangle() override {
        glBindVertexArray(m_emptyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        ++m_stats.drawCalls;
        ++m_stats.triangles;
    }

    // ---------------------------------------------------------- readback ---
    bool readPixels(RenderTargetHandle rt, int w, int h, std::vector<uint8_t>& rgba) override {
        if (rt && !m_targets.contains(rt.id)) return false;
        return readFbo(rt ? m_targets[rt.id].fbo : 0, w, h, rgba);
    }
    bool readFramebuffer(FramebufferHandle fb, int w, int h, std::vector<uint8_t>& rgba) override { return readFbo(fboOf(fb), w, h, rgba); }

    FrameStats& stats() override { return m_stats; }

private:
    bool readFbo(GLuint fbo, int w, int h, std::vector<uint8_t>& rgba) {
        rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // GL origin is bottom-left; flip to top-left.
        size_t row = static_cast<size_t>(w) * 4;
        std::vector<uint8_t> tmp(row);
        for (int y = 0; y < h / 2; ++y) {
            uint8_t* a = rgba.data() + static_cast<size_t>(y) * row;
            uint8_t* b = rgba.data() + static_cast<size_t>(h - 1 - y) * row;
            std::copy(a, a + row, tmp.data());
            std::copy(b, b + row, a);
            std::copy(tmp.data(), tmp.data() + row, b);
        }
        return glGetError() == 0;
    }
    static void applySampler(GLenum target, const SamplerDesc& s, bool hasMips) {
        GLint mag = static_cast<GLint>(s.filter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
        GLint min = mag;
        if (s.mipmaps && hasMips) min = static_cast<GLint>(s.filter == Filter::Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, min);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, mag);
        GLint wrap = static_cast<GLint>(s.wrap == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
        glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap);
        if (s.depthCompare) {
            glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, static_cast<GLint>(GL_COMPARE_REF_TO_TEXTURE));
            glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(GL_LEQUAL));
        }
    }
    GLuint compile(GLenum type, const std::string& src, const std::string& name) {
        GLuint s = glCreateShader(type);
        const char* p = src.c_str();
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            FLEDGE_LOG_ERROR("rhi", "Compile failed for shader '{}' ({}): {}", name, type == GL_VERTEX_SHADER ? "vs" : "fs", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }
    static void freeMesh(GlMesh& m) {
        if (m.ibo) glDeleteBuffers(1, &m.ibo);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.tbo) glDeleteBuffers(1, &m.tbo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        m = {};
    }
    static void freeTarget(GlTarget& t) {
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        if (t.color) glDeleteTextures(1, &t.color);
        if (t.depth) glDeleteRenderbuffers(1, &t.depth);
        t.fbo = t.color = t.depth = 0;
    }

    std::string m_desc;
    uint32_t m_next = 1;
    std::unordered_map<uint32_t, GlMesh> m_meshes;
    struct TransparentHash {
        using is_transparent = void;
        size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };
    struct TransparentEq {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };
    std::unordered_map<uint32_t, GLuint> m_programs;
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint, TransparentHash, TransparentEq>> m_uniformLocations;
    std::unordered_map<uint32_t, GlTexture> m_textures;
    std::unordered_map<uint32_t, GlTarget> m_targets;
    std::unordered_map<uint32_t, GLuint> m_fbos;
    std::unordered_map<uint32_t, GLuint> m_buffers;
    GLuint m_current = 0;
    GLuint m_emptyVao = 0;
    static constexpr int kMaxUnits = 16;
    std::array<uint32_t, kMaxUnits> m_unitTexture{};
    std::array<GLenum, kMaxUnits> m_unitTarget{};
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_fbAttachments;
    bool m_s3tc = false;
    int m_maxSamples = 1;
    FrameStats m_stats;
};

} // namespace

std::unique_ptr<RenderDevice> createGlDevice(GlProcLoader loader) {
    if (const char* missing = gl::loadFunctions(loader)) {
        FLEDGE_LOG_ERROR("rhi", "OpenGL function '{}' not available (need GL 3.3 core)", missing);
        return nullptr;
    }
    return std::make_unique<GlDevice>();
}

} // namespace fledge::rhi
