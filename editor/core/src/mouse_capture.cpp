#include "fledge/editor/mouse_capture.h"

namespace fledge::editor {

CaptureUpdate MouseCapture::update(const CaptureInputs& in) {
    CaptureMode prev = m_mode;
    CaptureUpdate out;

    // ---- play lock
    bool playActive = in.playing && !in.paused;
    if (!playActive || !in.lockDuringPlay) {
        m_playIntent = false;
    } else {
        if (!m_wasPlayActive) m_playIntent = in.autoCaptureOnStart && in.windowFocused && !in.popupOpen;  // user Play / Resume captures at once
        if (m_playIntent && prev == CaptureMode::PlayLock && in.escapePressed) {
            m_playIntent = false;
            out.escapeConsumed = true;
        }
        if (!in.windowFocused || in.popupOpen) m_playIntent = false;
        else if (!m_playIntent && in.leftPressed && in.viewportHovered && !in.gameWantsCursor && !out.escapeConsumed)
        {
            m_playIntent = true;  // click back into the game view
            out.clickConsumed = true;
        }
    }
    m_wasPlayActive = playActive;
    bool playLock = m_playIntent && !in.gameWantsCursor;

    // ---- camera drags (edit mode only)
    if (m_drag != DragButton::None) {
        bool held = (m_drag == DragButton::Right && in.rightDown) || (m_drag == DragButton::Middle && in.middleDown) ||
                    (m_drag == DragButton::AltLeft && in.leftDown);
        if (!held || in.playing || !in.windowFocused || in.popupOpen) m_drag = DragButton::None;
    } else if (!in.playing && in.windowFocused && !in.popupOpen && in.viewportHovered && !in.gizmoHot) {
        if (in.rightPressed) m_drag = DragButton::Right;
        else if (in.middlePressed) m_drag = DragButton::Middle;
        else if (in.leftPressed && in.altDown) m_drag = DragButton::AltLeft;
    }

    m_mode = playLock ? CaptureMode::PlayLock : (m_drag != DragButton::None ? CaptureMode::CameraDrag : CaptureMode::None);
    out.mode = m_mode;
    out.began = prev == CaptureMode::None && m_mode != CaptureMode::None;
    out.ended = prev != CaptureMode::None && m_mode == CaptureMode::None;
    out.showHint = m_mode == CaptureMode::PlayLock;
    out.showClickHint = playActive && in.lockDuringPlay && m_mode != CaptureMode::PlayLock && !in.gameWantsCursor;
    return out;
}

} // namespace fledge::editor
