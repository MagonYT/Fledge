#pragma once
// Time Machine: rewind & branch for play-in-editor.
//
// While playing, the session runs gameplay in fixed 1/60 s steps. It records the input of every step
// and takes a snapshot every `snapshotEvery` steps. A snapshot holds the scene plus the live Fledge Script
// script properties. Seeking restores the nearest earlier snapshot and re-simulates the recorded
// inputs up to the requested step. Branching drops the recorded future, so play continues from that
// point with whatever code is loaded now (e.g. after a script hot reload). The history is a ring
// buffer: the oldest snapshots and inputs are dropped once `memoryCap` bytes are used.
//
// Determinism covers scene state, Rotator components, physics (the Jolt state is part of each snapshot) and
// Fledge Script files whose state lives in properties.
// Private C++ member state of script objects is not captured (they restart from OnStart at a snapshot).
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/script/input_map.h"

namespace fledge {

class EditorSession;

class TimeMachine {
public:
    static constexpr float kStep = 1.0f / 60.0f;
    int snapshotEvery = 30;                 // steps (0.5 s)
    size_t memoryCap = 256ull * 1024 * 1024;  // bytes for snapshots + inputs
    bool enabled = true;

    void reset();
    // Session hooks
    void beginPlay(EditorSession& s);  // snapshot at step 0
    void afterStep(EditorSession& s, const script::InputFrame& input);  // one fixed step ran with `input`

    // Navigation (pauses play). Returns false if the step is outside the recorded range.
    bool seek(EditorSession& s, int64_t step);
    // Drop everything after the current step: play continues from here (with the code loaded now).
    void branchHere();

    int64_t firstStep() const { return m_inputs.empty() && m_snaps.empty() ? 0 : m_base; }
    int64_t lastStep() const { return m_base + static_cast<int64_t>(m_inputs.size()); }
    int64_t currentStep() const { return m_current; }
    bool atEnd() const { return m_current >= lastStep(); }
    size_t bytes() const { return m_bytes; }
    size_t snapshotCount() const { return m_snaps.size(); }
    int branches() const { return m_branches; }

private:
    struct Snap {
        int64_t step = 0;
        Json scene;
        Json scripts;  // [{entity, class, props}]
        Bytes physics;  // exact Jolt state (contacts, sleeping, solver warm start); empty without physics
        size_t bytes = 0;
    };
    Snap capture(EditorSession& s, int64_t step) const;
    void restore(EditorSession& s, const Snap& snap);
    void enforceCap();
    std::deque<Snap> m_snaps;
    std::deque<script::InputFrame> m_inputs;  // m_inputs[i] = input of step m_base + i
    int64_t m_base = 0, m_current = 0;
    size_t m_bytes = 0;
    int m_branches = 0;
};

} // namespace fledge
