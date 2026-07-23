#include "AYEditorGameView.h"
#include "AYEditorPlayRuntime.h"
#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"

namespace ayt::editor {

namespace {

void setRenderClockPaused(bool paused)
{
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        sub->renderer().setPostProcessClockPaused(paused);
    }
}

} // namespace

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
        setRenderClockPaused(false);
        _runtime.enterEdit();
        break;
    case EditorMode::Play:
        setRenderClockPaused(false);
        _runtime.startPlay();
        _runtime.tick();
        break;
    case EditorMode::Paused:
        if (!_runtime.isEngineInitialized()) {
            _runtime.startPlay();
        }
        ayt::game::GameLoop::instance().pause();
        setRenderClockPaused(true);
        break;
    }
}

} // namespace ayt::editor
