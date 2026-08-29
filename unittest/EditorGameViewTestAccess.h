#pragma once

#include "AYEditor/EditorGameView.h"

namespace ayt::editor {

// Narrow test seam for UI-state tests that must observe a mode-change callback
// without bootstrapping a native renderer window. Runtime transition tests use
// EditorGameView::trySetMode() instead.
struct EditorGameViewTestAccess {
    static void forceMode(EditorGameView& view, EditorMode mode)
    {
        view._mode = mode;
    }

    static void forceModeAndNotify(EditorGameView& view, EditorMode mode)
    {
        view._mode = mode;
        if (view._modeChanged) view._modeChanged(mode);
    }
};

} // namespace ayt::editor
