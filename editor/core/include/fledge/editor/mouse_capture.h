#pragma once
// Mouse capture state machine for the editor viewport (pure logic; the app applies the result to the window).
//
//   PlayLock    while playing in the editor (preference "Lock mouse during Play", default on): the cursor is
//               hidden and locked to the Game view, the game gets relative motion. Released on Stop, Pause,
//               Esc, focus loss and whenever a popup / modal opens; re-captured by clicking into the view.
//               While the game shows an interactive UI screen (it wants a cursor) the lock is suspended and
//               comes back by itself when the screen closes.
//   CameraDrag  while a viewport camera drag is held in edit mode (right = fly, middle = pan, Alt + left = orbit):
//               the cursor is hidden and moves relatively, so a turn never stops at the screen edge. On release
//               the cursor goes back to where the drag started.
namespace fledge::editor {

enum class CaptureMode { None, PlayLock, CameraDrag };
enum class DragButton { None, Right, Middle, AltLeft };

struct CaptureInputs {
    bool lockDuringPlay = true;   // preference
    bool autoCaptureOnStart = true; // this play session was started by the user (Play button / F5 / menu); MCP / AI / bot
                                    // sessions start with a free cursor (a click in the view still captures)
    bool playing = false;
    bool paused = false;
    bool windowFocused = true;
    bool popupOpen = false;       // any ImGui popup / modal / dialog
    bool gameWantsCursor = false; // an interactive UiScreen is visible
    bool viewportHovered = false; // over the game / scene view and not over an overlay widget
    bool escapePressed = false;
    bool leftPressed = false;     // pressed this frame
    bool rightDown = false, rightPressed = false;
    bool middleDown = false, middlePressed = false;
    bool leftDown = false, altDown = false;
    bool gizmoHot = false;        // the transform gizmo owns the click
};

struct CaptureUpdate {
    CaptureMode mode = CaptureMode::None;
    bool began = false;          // None -> captured this frame: hide + lock the cursor, remember where it was
    bool ended = false;          // captured -> None: show the cursor at the remembered position
    bool escapeConsumed = false; // Esc released the lock
    bool clickConsumed = false;  // the click that re-captured (not a game click)
    bool showHint = false;       // "Press Esc to release the mouse"
    bool showClickHint = false;  // playing, lock wanted but released by the user: "Click to capture the mouse"
};

class MouseCapture {
public:
    CaptureUpdate update(const CaptureInputs& in);
    CaptureMode mode() const { return m_mode; }
    DragButton dragButton() const { return m_drag; }
    void reset() { *this = MouseCapture{}; }

private:
    CaptureMode m_mode = CaptureMode::None;
    DragButton m_drag = DragButton::None;
    bool m_playIntent = false;      // the user wants the play lock (until Esc / focus loss / popup)
    bool m_wasPlayActive = false;
};

} // namespace fledge::editor
