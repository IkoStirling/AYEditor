#include "AYEditorGameView.h"
#include "AYEditorPlayRuntime.h"
#include "AYGameLoop.h"

namespace ayt::editor {

EditorGameView::EditorGameView(ayt::game::IGameLoop& loop, EditorPlayRuntime& runtime)
    : _loop(loop)
    , _runtime(runtime) {
}

void EditorGameView::setModeChangedCallback(ModeChangedCallback callback) {
    _modeChanged = std::move(callback);
}

void EditorGameView::setMode(EditorMode mode) {
    if (_mode == mode) {
        return;
    }

    applyMode(mode);
    _mode = mode;

    if (_modeChanged) {
        _modeChanged(_mode);
    }
}

void EditorGameView::stepOnce() {
    if (_mode == EditorMode::Paused) {
        _loop.stepOnce();
    }
}

void EditorGameView::applyMode(EditorMode mode) {
    switch (mode) {
    case EditorMode::Edit:
        _runtime.enterEdit();
        break;
    case EditorMode::Play:
        _runtime.startPlay();
        _runtime.tick();
        break;
    case EditorMode::Paused:
        if (!_runtime.isEngineInitialized()) {
            _runtime.startPlay();
        }
        ayt::game::GameLoop::instance().pause();
        break;
    }
}

} // namespace ayt::editor
