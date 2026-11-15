#pragma once

#include <memory>
#include <string_view>

#include "fledge/platform/window.h"
#include "fledge/render/renderer.h"

namespace fledge {
class FhtmlOverlay {
public:
    explicit FhtmlOverlay(rhi::RenderDevice& device);
    ~FhtmlOverlay();
    bool start(std::string_view html, const fs::path& workingDirectory);
    void update(const Window& window, bool inputEnabled);
    // Compact JSON written into the helper; the page reads it as fledge.state / fledge.onState.
    void setState(std::string_view json);
    rhi::TextureHandle texture() const;
    std::string takeAction();

private:
    struct State;
    std::unique_ptr<State> m;
};
} // namespace fledge
