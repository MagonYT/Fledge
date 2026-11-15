#include "fhtml_overlay.h"

#include "fhtml_shared.h"
#include "fledge/core/guid.h"
#include "fledge/core/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge {
struct FhtmlOverlay::State {
    explicit State(rhi::RenderDevice& renderDevice) : device(renderDevice) {}
    rhi::RenderDevice& device;
    rhi::TextureHandle image{};
    std::string action;
#ifdef _WIN32
    HANDLE mapping = nullptr, process = nullptr, job = nullptr;
    fhtml::Shared* shared = nullptr;
    uint32_t frameSerial = 0, actionSerial = 0, stateSerial = 0;
    Bytes pixels;
    std::string pendingState, lastState;
#endif
};

FhtmlOverlay::FhtmlOverlay(rhi::RenderDevice& device) : m(std::make_unique<State>(device)) {}
FhtmlOverlay::~FhtmlOverlay() {
#ifdef _WIN32
    if (m->shared) InterlockedExchange(&m->shared->stop, 1);
    if (m->process) {
        WaitForSingleObject(m->process, 2000);
        CloseHandle(m->process);
    }
    if (m->job) CloseHandle(m->job);  // terminate the helper tree if graceful shutdown timed out
    if (m->shared) UnmapViewOfFile(m->shared);
    if (m->mapping) CloseHandle(m->mapping);
#endif
    if (m->image) m->device.destroyTexture(m->image);
}

bool FhtmlOverlay::start(std::string_view html, const fs::path& workingDirectory) {
#ifdef _WIN32
    const auto helper = executablePath().parent_path() / "Fhtml" / "FledgeFhtml.exe";
    std::error_code ec;
    if (!fs::is_regular_file(osPath(helper), ec) || html.empty() || html.size() > fhtml::htmlCapacity) return false;
    fs::create_directories(osPath(workingDirectory), ec);
    if (ec) return false;
    const auto token = widen(Guid::generate().toString());
    const auto cache = (workingDirectory / token).u16string();
    if (cache.size() >= fhtml::cacheCapacity) return false;
    const auto name = L"Local\\FledgeFhtml-" + token;
    m->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(fhtml::Shared), name.c_str());
    if (!m->mapping || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    m->shared = static_cast<fhtml::Shared*>(MapViewOfFile(m->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fhtml::Shared)));
    if (!m->shared) return false;
    m->pixels.resize(sizeof(m->shared->bgra));
    std::memcpy(m->shared->html, html.data(), html.size());
    std::copy(cache.begin(), cache.end(), m->shared->cacheDirectory);
    m->shared->htmlSize = static_cast<uint32_t>(html.size());
    m->shared->mouseX = m->shared->mouseY = -1;
    m->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m->job || !SetInformationJobObject(m->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
    std::wstring command = L"\"" + helper.wstring() + L"\" --fledge-shared=" + name;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(osPath(helper).c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, osPath(workingDirectory).c_str(), &startup, &child)) return false;
    m->process = child.hProcess;
    const bool started = AssignProcessToJobObject(m->job, child.hProcess) && ResumeThread(child.hThread) != static_cast<DWORD>(-1);
    if (!started) TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hThread);
    return started;
#else
    (void)html;
    (void)workingDirectory;
    return false;
#endif
}

void FhtmlOverlay::update(const Window& window, bool inputEnabled) {
#ifdef _WIN32
    if (!m->process) return;
    if (WaitForSingleObject(m->process, 0) == WAIT_OBJECT_0) {
        FLEDGE_LOG_WARN("fhtml", "Browser helper exited. Game rendering continues.");
        CloseHandle(m->process);
        m->process = nullptr;
        if (m->image) m->device.destroyTexture(m->image);
        m->image = {};
        return;
    }
    if (InterlockedCompareExchange(&m->shared->lock, 1, 0) != 0) return;
    if (!m->pendingState.empty() && m->pendingState.size() < fhtml::stateCapacity) {
        std::memcpy(m->shared->state, m->pendingState.data(), m->pendingState.size());
        m->shared->state[m->pendingState.size()] = 0;
        m->shared->stateSize = static_cast<uint32_t>(m->pendingState.size());
        m->shared->stateSerial = ++m->stateSerial;
        m->pendingState.clear();
    }
    const auto& input = window.input();
    const float logicalWidth = window.width() / window.contentScale();
    const float logicalHeight = window.height() / window.contentScale();
    const bool inside = inputEnabled && logicalWidth > 0 && logicalHeight > 0 &&
                        input.mousePos.x >= 0 && input.mousePos.y >= 0 &&
                        input.mousePos.x < logicalWidth && input.mousePos.y < logicalHeight;
    m->shared->mouseX = inside ? static_cast<int>(input.mousePos.x * fhtml::width / logicalWidth) : -1;
    m->shared->mouseY = inside ? static_cast<int>(input.mousePos.y * fhtml::height / logicalHeight) : -1;
    m->shared->mouseDown = inside && input.mouseDown(MouseButton::Left);
    m->shared->keys = 0;
    if (inputEnabled) {
        for (size_t i = 0; i < std::size(fhtml::controlKeys); ++i)
            if (input.raw[fhtml::controlKeys[i]]) m->shared->keys |= 1u << i;
        m->shared->textSize = std::min(m->shared->textSize, static_cast<uint32_t>(sizeof(m->shared->text)));
        const size_t count = std::min(input.asciiText.size(), sizeof(m->shared->text) - m->shared->textSize);
        std::memcpy(m->shared->text + m->shared->textSize, input.asciiText.data(), count);
        m->shared->textSize += static_cast<uint32_t>(count);
    } else m->shared->textSize = 0;
    const bool frame = m->frameSerial != m->shared->serial;
    const bool first = m->frameSerial == 0;
    if (frame) {
        m->frameSerial = m->shared->serial;
        std::memcpy(m->pixels.data(), m->shared->bgra, m->pixels.size());
    }
    char action[sizeof(m->shared->action)]{};
    if (m->actionSerial != m->shared->actionSerial) {
        m->actionSerial = m->shared->actionSerial;
        std::memcpy(action, m->shared->action, sizeof(action) - 1);
    }
    InterlockedExchange(&m->shared->lock, 0);
    if (action[0]) {
        m->action = action;
        FLEDGE_LOG_INFO("fhtml", "Page event: {}", action);
    }
    if (!frame) return;
    for (size_t i = 0; i < m->pixels.size(); i += 4) {
        std::swap(m->pixels[i], m->pixels[i + 2]);
        const unsigned alpha = m->pixels[i + 3];
        if (alpha == 0 || alpha == 255) continue;
        for (size_t channel = 0; channel < 3; ++channel)
            m->pixels[i + channel] = static_cast<uint8_t>(std::min(255u, (m->pixels[i + channel] * 255u + alpha / 2) / alpha));
    }
    const rhi::TextureSubresource pixels{0, 0, fhtml::width, fhtml::height, m->pixels.data(), m->pixels.size()};
    if (m->image && m->device.updateTexture(m->image, pixels)) {
        if (first) FLEDGE_LOG_INFO("fhtml", "Browser frame ready ({}x{})", fhtml::width, fhtml::height);
    } else {
        rhi::TextureDesc desc;
        desc.width = fhtml::width;
        desc.height = fhtml::height;
        auto texture = m->device.createTexture(desc, {&pixels, 1});
        if (texture) {
            if (m->image) m->device.destroyTexture(m->image);
            m->image = texture;
            if (first) FLEDGE_LOG_INFO("fhtml", "Browser frame ready ({}x{})", desc.width, desc.height);
        }
    }
#else
    (void)window;
    (void)inputEnabled;
#endif
}

rhi::TextureHandle FhtmlOverlay::texture() const { return m->image; }
std::string FhtmlOverlay::takeAction() { return std::exchange(m->action, {}); }

void FhtmlOverlay::setState(std::string_view json) {
#ifdef _WIN32
    if (!m->shared || json.size() >= fhtml::stateCapacity) return;
    if (json == m->lastState) return;
    m->lastState.assign(json.begin(), json.end());
    m->pendingState = m->lastState;
#else
    (void)json;
#endif
}
} // namespace fledge
