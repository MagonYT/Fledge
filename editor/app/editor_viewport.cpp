// Viewport: editor camera (fly/orbit/pan), picking, transform gizmo, shortcuts.
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

#include "fledge/core/log.h"
#include "fledge/scene/components.h"
#include "editor_app.h"
#include "editor_icons.h"

namespace fledge::editor {

namespace {
ImTextureID toImTex(uint64_t glTex) { return static_cast<ImTextureID>(glTex); }

const ImU32 kAxisColors[3] = {IM_COL32(230, 60, 60, 255), IM_COL32(90, 210, 70, 255), IM_COL32(70, 120, 240, 255)};
const ImU32 kHotColor = IM_COL32(255, 220, 40, 255);

float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    float px = a.x + t * dx - p.x, py = a.y + t * dy - p.y;
    return std::sqrt(px * px + py * py);
}
} // namespace

ViewItem EditorApp::editorView(float aspect) const {
    ViewItem v;
    Quat q = Quat::fromEuler({m_camPitch, m_camYaw, 0});
    v.position = m_camPos;
    v.view = Mat4::lookAt(m_camPos, m_camPos + q.rotate({0, 0, -1}), {0, 1, 0});
    v.projection = Mat4::perspective(radians(60), aspect, 0.05f, 1000);
    return v;
}

// ------------------------------------------------------------------ mouse capture --
// While captured the OS cursor is disabled (hidden, relative motion, raw input where supported). ImGui's pointer is
// pinned to the anchor inside the view, so it never sees a bogus hover / drag elsewhere; the motion goes through
// m_captureDelta to the editor camera and the game instead.
ImVec2 EditorApp::viewportMouseDelta() const {
    return m_capture.mode() != CaptureMode::None ? m_captureDelta : ImGui::GetIO().MouseDelta;
}

void EditorApp::pinCapturedMouse() {
    GLFWwindow* gw = m_window ? m_window->glfwHandle() : nullptr;
    if (!gw || m_capture.mode() == CaptureMode::None) {
        m_captureDelta = {0, 0};
        return;
    }
    double x = 0, y = 0;
    glfwGetCursorPos(gw, &x, &y);
    m_captureDelta = {static_cast<float>(x - m_captureLastX), static_cast<float>(y - m_captureLastY)};
    m_captureLastX = x;
    m_captureLastY = y;
    ImGui::GetIO().AddMousePosEvent(m_captureAnchor.x, m_captureAnchor.y);  // the last position event of the frame wins
}

void EditorApp::updateMouseCapture() {
    ImGuiIO& io = ImGui::GetIO();
    GLFWwindow* gw = m_window ? m_window->glfwHandle() : nullptr;
    // Who started this play session: only the user's Play (button, F5, menu, Simple Mode) auto-locks the mouse.
    // MCP / AI assistant / playtest bot sessions call EditorSession::play() directly and start with a free cursor.
    bool playingNow = m_session.playing();
    if (playingNow && !m_capturePlayingSeen) m_userStartedPlay = m_userPlayPending;
    if (!playingNow) m_userStartedPlay = false;
    if (m_userPlayPending && (playingNow != m_capturePlayingSeen || ++m_userPlayPendingAge > 2)) {
        m_userPlayPending = false;  // consumed by the transition, or the play request failed
        m_userPlayPendingAge = 0;
    }
    m_capturePlayingSeen = playingNow;
    if (!gw) {  // headless: never grab the real mouse
        m_captureState = {};
        return;
    }
    CaptureInputs in;
    in.lockDuringPlay = m_prefs.lockMouseInPlay;
    in.autoCaptureOnStart = m_userStartedPlay;
    in.playing = m_session.playing();
    in.paused = m_session.paused();
    in.windowFocused = glfwGetWindowAttrib(gw, GLFW_FOCUSED) == GLFW_TRUE;
    in.popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    in.gameWantsCursor = in.playing && m_session.world().wantsCursor();
    in.viewportHovered = m_vpHovered && !m_overlayHovered;
    in.escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    in.leftPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    in.leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    in.rightPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    in.rightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    in.middlePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    in.middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    in.altDown = io.KeyAlt;
    in.gizmoHot = m_hoverAxis >= 0 || m_drag.active();
    m_captureState = m_capture.update(in);
    if (m_captureState.clickConsumed) m_suppressGameLeft = true;
    if (m_captureState.began) {
        glfwGetCursorPos(gw, &m_captureRestoreX, &m_captureRestoreY);
        // Drags pin ImGui where the button went down; the play lock pins inside the view (centre when it started
        // from the toolbar), so the game view stays "hovered".
        m_captureAnchor = (m_captureState.mode == CaptureMode::CameraDrag || in.viewportHovered)
                              ? io.MousePos
                              : ImVec2{m_vpOrigin.x + m_vpSize.x * 0.5f, m_vpOrigin.y + m_vpSize.y * 0.5f};
        glfwSetInputMode(gw, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(gw, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        glfwGetCursorPos(gw, &m_captureLastX, &m_captureLastY);
        m_captureDelta = {0, 0};
    } else if (m_captureState.ended) {
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(gw, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(gw, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPos(gw, m_captureRestoreX, m_captureRestoreY);  // back where the drag / lock started
        io.AddMousePosEvent(static_cast<float>(m_captureRestoreX), static_cast<float>(m_captureRestoreY));
        m_captureDelta = {0, 0};
    }
}

void EditorApp::updateEditorCamera(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 md = viewportMouseDelta();
    Quat q = Quat::fromEuler({m_camPitch, m_camYaw, 0});
    Vec3 fwd = q.rotate({0, 0, -1}), right = q.rotate({1, 0, 0}), up = q.rotate({0, 1, 0});
    Vec3 pivot = m_camPos + fwd * m_camDist;
    if (!m_vpHovered && !m_drag.active()) return;
    if (m_overlayHovered) return;
    float sens = m_prefs.mouseSensitivity, inv = m_prefs.invertY ? -1.0f : 1.0f;
    if (m_vpHovered && io.MouseWheel != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {  // dolly towards the pivot
        float step = std::max(0.2f, m_camDist * 0.15f) * io.MouseWheel;
        m_camDist = std::max(0.3f, m_camDist - step);
        m_camPos = pivot - fwd * m_camDist;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {  // fly
        m_camYaw -= md.x * 0.005f * sens;
        m_camPitch = std::clamp(m_camPitch - md.y * 0.005f * sens * inv, radians(-89), radians(89));
        float speed = m_prefs.cameraSpeed * (io.KeyShift ? 3.0f : 1.0f) * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) m_camPos += fwd * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) m_camPos -= fwd * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) m_camPos += right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) m_camPos -= right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) m_camPos += Vec3{0, speed, 0};
        if (ImGui::IsKeyDown(ImGuiKey_Q)) m_camPos -= Vec3{0, speed, 0};
        if (io.MouseWheel != 0) m_prefs.cameraSpeed = std::clamp(m_prefs.cameraSpeed * (io.MouseWheel > 0 ? 1.2f : 1 / 1.2f), 0.5f, 50.0f);
    } else if (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // orbit around the pivot
        m_camYaw -= md.x * 0.006f * sens;
        m_camPitch = std::clamp(m_camPitch - md.y * 0.006f * sens * inv, radians(-89), radians(89));
        Vec3 nf = Quat::fromEuler({m_camPitch, m_camYaw, 0}).rotate({0, 0, -1});
        m_camPos = pivot - nf * m_camDist;
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {  // pan
        float k = m_camDist * 0.0015f;
        m_camPos += right * (-md.x * k) + up * (md.y * k);
    }
}

void EditorApp::setCameraView(Vec3 forward) {
    Vec3 fwd = Quat::fromEuler({m_camPitch, m_camYaw, 0}).rotate({0, 0, -1});
    Vec3 pivot = m_camPos + fwd * m_camDist;
    Vec3 f = normalize(forward);
    m_camPitch = std::clamp(std::asin(std::clamp(f.y, -1.0f, 1.0f)), radians(-89.9f), radians(89.9f));
    if (std::fabs(f.y) < 0.999f) m_camYaw = std::atan2(-f.x, -f.z);
    Vec3 nf = Quat::fromEuler({m_camPitch, m_camYaw, 0}).rotate({0, 0, -1});
    m_camPos = pivot - nf * m_camDist;
}

void EditorApp::focusSelection() {
    EntityId e = m_session.primarySelection();
    if (!m_session.scene().isValid(e)) return;
    Mat4 m = m_session.scene().worldMatrix(e);
    Vec3 center = m.c[3].xyz();
    float radius = 0.5f;
    if (auto* mr = m_session.scene().get<MeshRendererComponent>(e))
        if (const MeshData* md = m_session.world().cpuMesh(mr->mesh)) {
            Vec3 mn, mx;
            meshBounds(*md, mn, mx);
            Vec3 a = m.transformPoint(mn), b = m.transformPoint(mx);
            center = (a + b) * 0.5f;
            radius = std::max(0.3f, length(b - a) * 0.5f);
        }
    m_camDist = std::max(1.5f, radius * 3.0f);
    Vec3 fwd = Quat::fromEuler({m_camPitch, m_camYaw, 0}).rotate({0, 0, -1});
    m_camPos = center - fwd * m_camDist;
}

Ray EditorApp::mouseRay(const ViewItem& view, ImVec2 origin, ImVec2 size) const {
    ImVec2 mp = ImGui::GetIO().MousePos;
    float nx = (mp.x - origin.x) / size.x * 2.0f - 1.0f;
    float ny = 1.0f - (mp.y - origin.y) / size.y * 2.0f;
    return screenRay(nx, ny, view.projection * view.view);
}

Result<ImageData> EditorApp::captureViewport(int w, int h) {
    auto rt = m_device->createRenderTarget(w, h);
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    auto view = m_session.playing() ? m_session.world().primaryView(aspect) : std::nullopt;
    m_session.world().render(rt, w, h, view ? *view : editorView(aspect), {}, !m_session.playing());
    ImageData img;
    img.width = w;
    img.height = h;
    bool ok = m_device->readPixels(rt, w, h, img.rgba);
    m_device->destroyRenderTarget(rt);
    if (!ok) return Result<ImageData>::failure("GPU readback failed");
    return img;
}

void EditorApp::drawViewport(float dt) {
    PanelDef* pd = panel("Viewport");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    bool visible = ImGui::Begin("Viewport", pd ? &pd->open : nullptr);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        m_vpHovered = false;
        m_vpFocused = false;
        if (m_session.playing() && !m_session.scriptedInputOnly) m_session.liveInput = {};
        return;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = std::max(1, static_cast<int>(avail.x)), h = std::max(1, static_cast<int>(avail.y));
    if (w != m_vpW || h != m_vpH) {
        m_device->resizeRenderTarget(m_viewportRt, w, h);
        m_vpW = w;
        m_vpH = h;
    }
    m_vpHovered = ImGui::IsWindowHovered();
    m_vpFocused = ImGui::IsWindowFocused();
    updateMouseCapture();
    feedPlayInput();
    updateEditorCamera(dt);
    float aspect = static_cast<float>(w) / static_cast<float>(h);
    std::optional<ViewItem> view;
    bool playing = m_session.playing();
    if (playing) view = m_session.netView(aspect);
    if (playing && !view) view = m_session.world().primaryView(aspect);
    if (!view) view = editorView(aspect);
    m_lastView = *view;
    // The view mode is a viewport setting: swap it in for this render only (project render settings stay untouched).
    DebugView projectView = m_session.world().renderSettings.debugView;
    m_session.world().renderSettings.debugView = m_viewMode;
    worldFrameHooks();
    uint64_t idleKey = 14695981039346656037ull;
    auto mix = [&](uint64_t v) {
        idleKey ^= v;
        idleKey *= 1099511628211ull;
    };
    mix(playing ? 1ull : 0ull);
    mix(m_session.history().cursor());
    mix(m_session.history().commands().size());
    mix(m_session.selection.size());
    if (!m_session.selection.empty()) mix(static_cast<uint64_t>(m_session.selection.front()));
    mix(static_cast<uint64_t>(m_viewMode));
    mix(static_cast<uint64_t>(w) ^ (static_cast<uint64_t>(h) << 20));
    mix(m_prefs.showGrid ? 1ull : 0ull);
    mix(static_cast<uint64_t>(std::llround(view->position.x * 200.0)));
    mix(static_cast<uint64_t>(std::llround(view->position.y * 200.0)));
    mix(static_cast<uint64_t>(std::llround(view->position.z * 200.0)));
    mix(static_cast<uint64_t>(std::llround(m_camYaw * 200.0)));
    mix(static_cast<uint64_t>(std::llround(m_camPitch * 200.0)));
    bool preview = false;
    for (EntityId e : m_session.scene().pool<AnimatorComponent>().entities())
        if (auto* ac = m_session.scene().get<AnimatorComponent>(e); ac && ac->preview) preview = true;
    bool skipDraw = !playing && !preview && m_vpIdleValid && idleKey == m_vpIdleKey;
    m_vpIdleKey = idleKey;
    m_vpIdleValid = true;
    if (!skipDraw)
        m_session.world().render(m_viewportRt, w, h, *view, playing ? std::vector<EntityId>{} : m_session.selection, !playing && m_prefs.showGrid);
    m_session.world().renderSettings.debugView = projectView;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    m_vpOrigin = origin;
    m_vpSize = {static_cast<float>(w), static_cast<float>(h)};
    const bool bottomUp = m_device->backend() == rhi::Backend::OpenGL;
    ImGui::Image(toImTex(m_device->nativeColorTexture(m_viewportRt)), avail, {0, bottomUp ? 1.0f : 0.0f}, {1, bottomUp ? 0.0f : 1.0f});

    if (!playing && ImGui::BeginDragDropTarget()) {  // drop assets from the Content Browser into the world
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            Ray ray = mouseRay(*view, origin, m_vpSize);
            PickHit hit = pickEntity(m_session.scene(), [this](const std::string& ref) { return m_session.world().cpuMesh(ref); }, ray);
            float t = 0;
            Vec3 at = hit.entity ? hit.point : (rayPlane(ray, {0, 0, 0}, {0, 1, 0}, t) ? ray.at(t) : ray.at(5.0f));
            dropAsset(static_cast<const char*>(pl->Data), hit.entity, at);
        }
        ImGui::EndDragDropTarget();
    }
    drawViewportOverlays(*view, origin, m_vpSize);
    if (playing) drawTimeMachineBar(origin, m_vpSize);
    if (m_captureState.showHint || m_captureState.showClickHint) {  // mouse lock hint (bottom centre)
        const char* hint = m_captureState.showHint ? "Press Esc to release the mouse" : "Click in the view to capture the mouse";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        float s = effectiveScale();
        ImVec2 p{origin.x + (m_vpSize.x - ts.x) * 0.5f, origin.y + m_vpSize.y - ts.y - 56 * s};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({p.x - 10 * s, p.y - 5 * s}, {p.x + ts.x + 10 * s, p.y + ts.y + 5 * s}, IM_COL32(0, 0, 0, 150), 6 * s);
        dl->AddText(p, IM_COL32(255, 255, 255, 230), hint);
    }
    bool brushing = !playing && !m_overlayHovered && updateTerrainBrush(*view, origin, m_vpSize);
    if (!playing && !m_overlayHovered && !brushing) drawGizmo(*view, origin, m_vpSize);

    // Click to select (after the gizmo and the overlays had their chance to grab the mouse).
    ImGuiIO& io = ImGui::GetIO();
    if (!playing && !brushing && m_vpHovered && !m_overlayHovered && !io.KeyAlt && !m_drag.active() && m_hoverAxis < 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Ray ray = mouseRay(*view, origin, m_vpSize);
        PickHit hit = pickEntity(m_session.scene(), [this](const std::string& ref) { return m_session.world().cpuMesh(ref); }, ray);
        m_session.select(hit.entity, io.KeyCtrl);
        if (hit.entity) m_camDist = std::max(0.5f, hit.distance);  // orbit around what was clicked
    }
    ImGui::End();
}

void EditorApp::drawViewportOverlays(const ViewItem& view, ImVec2 origin, ImVec2 size) {
    float s = effectiveScale();
    ImGuiIO& io = ImGui::GetIO();
    bool hovered = false;
    // ---- overlay toolbar (top-left)
    ImGui::SetCursorScreenPos({origin.x + 8 * s, origin.y + 8 * s});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * s);
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(bg.x, bg.y, bg.z, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(bg.x, bg.y, bg.z, 0.78f));
    const char* modeNames[] = {"Lit", "Albedo", "Normals", "Roughness", "Metallic", "Depth", "Shadow Cascades", "Wireframe"};
    int mode = static_cast<int>(m_viewMode);
    ImGui::SetNextItemWidth(130 * s);
    std::string preview = std::string(ICON_EYE "  ") + modeNames[mode];
    if (ImGui::BeginCombo("##viewmode", preview.c_str())) {
        for (int i = 0; i < 8; ++i)
            if (ImGui::Selectable(modeNames[i], i == mode)) m_viewMode = static_cast<DebugView>(i);
        ImGui::EndCombo();
    }
    hovered |= ImGui::IsItemHovered() || ImGui::IsItemActive();
    trackRect("vp.viewmode");
    ImGui::SameLine(0, 4 * s);
    auto toggle = [&](const char* id, const char* icon, bool on, const char* tip) {
        ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, a);
        bool pressed = ImGui::Button(icon);
        if (on) ImGui::PopStyleColor();
        trackRect(id);
        hovered |= ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, 4 * s);
        return pressed;
    };
    if (toggle("vp.grid", ICON_GRID, m_prefs.showGrid, "Grid (G)")) m_prefs.showGrid = !m_prefs.showGrid;
    if (toggle("vp.stats", ICON_GAUGE, m_prefs.showStats, "Stats overlay")) m_prefs.showStats = !m_prefs.showStats;
    if (toggle("vp.cube", ICON_CUBE, m_prefs.showOrientation, "Orientation cube")) m_prefs.showOrientation = !m_prefs.showOrientation;
    std::string speed = std::format("{} {:.1f} m/s", ICON_VIDEO, m_prefs.cameraSpeed);
    if (ImGui::Button(speed.c_str())) ImGui::OpenPopup("camspeed");
    hovered |= ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Camera fly speed (RMB + wheel also changes it)");
    if (ImGui::BeginPopup("camspeed")) {
        ImGui::SetNextItemWidth(180 * s);
        ImGui::SliderFloat("Fly speed", &m_prefs.cameraSpeed, 0.5f, 50.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(180 * s);
        ImGui::SliderFloat("Sensitivity", &m_prefs.mouseSensitivity, 0.2f, 3.0f, "%.2fx");
        ImGui::Checkbox("Invert Y", &m_prefs.invertY);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    hovered |= ImGui::IsPopupOpen("camspeed") || ImGui::IsPopupOpen("##viewmode");

    // ---- stats (below the toolbar)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (m_prefs.showStats) {
        auto st = m_device->stats();
        bool playing = m_session.playing();
        std::string lines = std::format("{}\n{:.0f} FPS  {:.2f} ms\n{} draws  {} tris\n{}x{}  {} entities",
                                        playing ? (m_session.paused() ? "PAUSED" : "PLAYING") : "Editor camera", io.Framerate, m_frameMs,
                                        st.drawCalls, st.triangles, static_cast<int>(size.x), static_cast<int>(size.y),
                                        m_session.scene().entities().size());
        const auto& rs = m_session.world().renderStats();
        const auto& fi = m_renderer->lastFrame();
        lines += std::format("\nmeshes {}/{}  culled: frustum {} occl {} dist {}\nLOD 0-3: {} {} {} {}  instanced {} in {} draws{}", rs.drawn, rs.candidates,
                             rs.frustumCulled, rs.occlusionCulled, rs.distanceCulled, rs.lodHistogram[0], rs.lodHistogram[1], rs.lodHistogram[2],
                             rs.lodHistogram[3] + rs.lodHistogram[4], fi.instancedItems, fi.instancedDraws,
                             rs.hlodDrawn ? std::format("  HLOD {}", rs.hlodDrawn) : std::string());
        lines += std::format("\nshadows {} cached / {} cascades  skinned {}  proxies {}\nlights {}  GI {}", fi.shadowCacheHits, fi.cascades,
                             fi.gpuSkinned, rs.shadowProxies, fi.clusteredLights, fi.giProbes ? "probes" : "off");
        if (fi.nonFiniteHdr >= 0)  // 1.3.3 counter (FLEDGE_COUNT_NONFINITE=1): pixels with NaN / Inf in the HDR image
            lines += std::format("\nNaN/Inf pixels: scene {} (Inf {})  post {}", fi.nonFiniteHdr, fi.nonFiniteHdrInf, fi.nonFinitePost);
        ImVec2 p{origin.x + 8 * s, origin.y + 8 * s + ImGui::GetFrameHeight() + 6 * s};
        ImVec2 ts = ImGui::CalcTextSize(lines.c_str());
        dl->AddRectFilled(p, {p.x + ts.x + 14 * s, p.y + ts.y + 10 * s}, IM_COL32(0, 0, 0, 120), 4 * s);
        dl->AddText({p.x + 7 * s, p.y + 5 * s}, IM_COL32(235, 235, 235, 235), lines.c_str());
    }
    if (!m_session.playing() && size.y > 200 * s) {
        const char* hint = "LMB select  Ctrl+LMB multi  RMB+WASD fly  Alt+LMB orbit  MMB pan  F focus  W/E/R tools  Ctrl+P search";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        if (ts.x + 20 < size.x)
            dl->AddText({origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y - ts.y - 8 * s}, IM_COL32(255, 255, 255, 110), hint);
    }
    // ---- orientation cube (top-right)
    if (m_prefs.showOrientation) {
        float r = 46 * s;
        ImVec2 c{origin.x + size.x - r - 18 * s, origin.y + r + 18 * s};
        if (io.MousePos.x > c.x - r * 1.3f && io.MousePos.x < c.x + r * 1.3f && io.MousePos.y > c.y - r * 1.3f && io.MousePos.y < c.y + r * 1.3f)
            hovered = true;
        m_overlayHovered = hovered;  // the cube reads it to decide whether it owns the click
        drawOrientationCube(view, c);
    }
    m_overlayHovered = hovered;
}

void EditorApp::drawOrientationCube(const ViewItem& view, ImVec2 c) {
    float s = effectiveScale();
    float half = 24 * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // View rotation only (orthographic): view-space = R * world, camera looks down -Z, so +z faces the viewer.
    auto rot = [&](Vec3 v) {
        const Mat4& m = view.view;
        return Vec3{m.c[0].x * v.x + m.c[1].x * v.y + m.c[2].x * v.z, m.c[0].y * v.x + m.c[1].y * v.y + m.c[2].y * v.z,
                    m.c[0].z * v.x + m.c[1].z * v.y + m.c[2].z * v.z};
    };
    auto toScreen = [&](Vec3 v) { return ImVec2{c.x + v.x * half, c.y - v.y * half}; };
    struct Face {
        Vec3 n, u, v;
        const char* label;
        ImU32 col;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, "Right", IM_COL32(205, 70, 70, 235)},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, "Left", IM_COL32(130, 50, 50, 235)},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}, "Top", IM_COL32(80, 175, 70, 235)},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, "Bottom", IM_COL32(50, 110, 45, 235)},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, "Front", IM_COL32(70, 110, 215, 235)},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, "Back", IM_COL32(45, 70, 140, 235)},
    };
    int order[6] = {0, 1, 2, 3, 4, 5};
    std::sort(order, order + 6, [&](int a, int b) { return rot(faces[a].n).z < rot(faces[b].n).z; });
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot = -1;
    ImVec2 quads[6][4];
    for (int k = 0; k < 6; ++k) {
        const Face& f = faces[k];
        Vec3 corners[4] = {f.n - f.u - f.v, f.n + f.u - f.v, f.n + f.u + f.v, f.n - f.u + f.v};
        for (int i = 0; i < 4; ++i) quads[k][i] = toScreen(rot(corners[i]));
    }
    auto inside = [&](int k) {
        float sign = 0;
        for (int i = 0; i < 4; ++i) {
            ImVec2 a = quads[k][i], b = quads[k][(i + 1) % 4];
            float cr = (b.x - a.x) * (mouse.y - a.y) - (b.y - a.y) * (mouse.x - a.x);
            if (std::fabs(cr) < 1e-6f) continue;
            if (sign == 0) sign = cr;
            else if ((cr > 0) != (sign > 0)) return false;
        }
        return true;
    };
    for (int i = 5; i >= 0; --i)  // front-most first
        if (rot(faces[order[i]].n).z > 0.05f && inside(order[i])) {
            hot = order[i];
            break;
        }
    dl->AddCircleFilled(c, half * 2.0f, IM_COL32(0, 0, 0, 60), 32);
    for (int idx : order) {
        const Face& f = faces[idx];
        float z = rot(f.n).z;
        if (z <= 0.0f) continue;
        ImU32 col = idx == hot ? IM_COL32(255, 200, 60, 245) : f.col;
        dl->AddQuadFilled(quads[idx][0], quads[idx][1], quads[idx][2], quads[idx][3], col);
        dl->AddQuad(quads[idx][0], quads[idx][1], quads[idx][2], quads[idx][3], IM_COL32(20, 20, 24, 255), 1.0f);
        if (z > 0.35f) {
            ImVec2 fc = toScreen(rot(f.n));
            ImVec2 ts = ImGui::CalcTextSize(f.label);
            float k = std::clamp((z - 0.35f) / 0.4f, 0.0f, 1.0f);
            dl->AddText({fc.x - ts.x * 0.5f, fc.y - ts.y * 0.5f}, IM_COL32(255, 255, 255, static_cast<int>(240 * k)), f.label);
        }
    }
    // axis tips
    const char* axisNames[3] = {"X", "Y", "Z"};
    const ImU32 axisCols[3] = {IM_COL32(240, 90, 90, 255), IM_COL32(110, 220, 90, 255), IM_COL32(90, 140, 250, 255)};
    for (int a = 0; a < 3; ++a) {
        Vec3 d{a == 0 ? 1.6f : 0.0f, a == 1 ? 1.6f : 0.0f, a == 2 ? 1.6f : 0.0f};
        ImVec2 tip = toScreen(rot(d));
        dl->AddLine(c, tip, axisCols[a], 1.5f * s);
        dl->AddText({tip.x - 3 * s, tip.y - 7 * s}, axisCols[a], axisNames[a]);
    }
    if (hot >= 0 && m_vpHovered) {
        ImGui::SetTooltip("%s view (click)", faces[hot].label);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) setCameraView(-faces[hot].n);
    }
}

void EditorApp::drawGizmo(const ViewItem& view, ImVec2 origin, ImVec2 size) {
    Scene& scene = m_session.scene();
    EntityId e = m_session.primarySelection();
    if (!scene.isValid(e)) {
        m_hoverAxis = -1;
        m_drag.end();
        return;
    }
    Mat4 vp = view.projection * view.view;
    Transform world = scene.worldTransform(e);
    Vec3 center = world.position;
    float len = std::max(0.05f, length(center - view.position) * 0.16f);
    GizmoSpace space = m_gizmo.mode == GizmoMode::Scale ? GizmoSpace::Local : m_gizmo.space;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto toScreen = [&](Vec3 p, ImVec2& out) {
        Vec2 s;
        if (!projectToScreen(vp, p, size.x, size.y, s)) return false;
        out = {origin.x + s.x, origin.y + s.y};
        return true;
    };
    ImVec2 c2;
    if (!toScreen(center, c2)) return;
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // Hover test + draw
    int hover = -1;
    float best = 9.0f;
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 dir = gizmoAxis(axis, space, world.rotation);
        bool hot = (m_drag.active() ? m_drag.axis() : m_hoverAxis) == axis;
        ImU32 col = hot ? kHotColor : kAxisColors[axis];
        if (m_gizmo.mode == GizmoMode::Rotate) {
            Vec3 u = normalize(cross(dir, std::fabs(dir.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
            Vec3 v = cross(dir, u);
            ImVec2 prev;
            bool havePrev = false;
            for (int i = 0; i <= 48; ++i) {
                float a = static_cast<float>(i) / 48.0f * 2.0f * kPi;
                ImVec2 p;
                if (!toScreen(center + (u * std::cos(a) + v * std::sin(a)) * len, p)) {
                    havePrev = false;
                    continue;
                }
                if (havePrev) {
                    dl->AddLine(prev, p, col, hot ? 3.5f : 2.0f);
                    float d = distToSegment(mouse, prev, p);
                    if (d < best) { best = d; hover = axis; }
                }
                prev = p;
                havePrev = true;
            }
        } else {
            ImVec2 tip;
            if (!toScreen(center + dir * len, tip)) continue;
            dl->AddLine(c2, tip, col, hot ? 4.0f : 2.5f);
            if (m_gizmo.mode == GizmoMode::Translate) dl->AddCircleFilled(tip, hot ? 7.0f : 5.5f, col);
            else dl->AddRectFilled({tip.x - 5, tip.y - 5}, {tip.x + 5, tip.y + 5}, col);
            float d = distToSegment(mouse, c2, tip);
            if (d < best) { best = d; hover = axis; }
        }
    }
    dl->AddCircleFilled(c2, 3.0f, IM_COL32(255, 255, 255, 200));

    ImGuiIO& io = ImGui::GetIO();
    if (!m_drag.active()) {
        m_hoverAxis = (m_vpHovered && !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) ? hover : -1;
        if (m_hoverAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Vec3 viewDir = normalize(center - view.position);
            m_drag.begin(m_gizmo, m_hoverAxis, world, mouseRay(view, origin, size), viewDir);
            // Move every selected entity whose ancestors are not also selected.
            m_dragStart.clear();
            for (EntityId s : m_session.selection) {
                bool nested = false;
                for (EntityId o : m_session.selection)
                    if (o != s && scene.isAncestor(o, s)) nested = true;
                if (!nested && scene.isValid(s)) m_dragStart.emplace_back(s, scene.worldTransform(s));
            }
        }
        return;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {  // drag finished
        m_drag.end();
        m_session.history().closeMerge();
        return;
    }
    // Apply this frame's delta (relative to the primary's start) to every dragged entity, as ONE merged command.
    Transform primaryNew = m_drag.update(mouseRay(view, origin, size));
    Transform primaryStart;
    for (auto& [id, t] : m_dragStart)
        if (id == e) primaryStart = t;
    Quat dq = (primaryNew.rotation * primaryStart.rotation.conjugate()).normalized();
    const char* label = m_gizmo.mode == GizmoMode::Translate ? "Move" : m_gizmo.mode == GizmoMode::Rotate ? "Rotate" : "Scale";
    m_session.history().begin(std::format("{} {} entit{}", label, m_dragStart.size(), m_dragStart.size() == 1 ? "y" : "ies"),
                              EditSource::User, "gizmo");
    for (auto& [id, start] : m_dragStart) {
        Transform t = start;
        if (id == e) t = primaryNew;
        else if (m_gizmo.mode == GizmoMode::Translate) t.position = start.position + (primaryNew.position - primaryStart.position);
        else if (m_gizmo.mode == GizmoMode::Rotate) {
            t.rotation = (dq * start.rotation).normalized();
            t.position = primaryStart.position + dq.rotate(start.position - primaryStart.position);
        } else {
            for (int k = 0; k < 3; ++k)
                if (std::fabs(primaryStart.scale[k]) > 1e-6f) t.scale[k] = start.scale[k] * primaryNew.scale[k] / primaryStart.scale[k];
        }
        (void)m_session.setWorldTransform(id, t);
    }
    m_session.history().commit();
}

} // namespace fledge::editor
