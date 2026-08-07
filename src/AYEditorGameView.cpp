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
    // v0.4 PR-1 (design §6): applyMode 是 EditorMode → EditorPlayRuntime
    // 的 adapter。**不直接**调 SceneManager — SM 入口由
    // _runtime.startPlay()/enterEdit() 头部接管 (F3.a / F3.b；G1/G5 收口)。
    // 链路：btn_play → applyMode(Play) → _runtime.startPlay() →
    // sm->beginPlay() → spawn → GameLoop::tickOnce (renderer 帧提交
    // 在 GameLoop 内耦合；PR-1 不切 SM::tick)。
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
