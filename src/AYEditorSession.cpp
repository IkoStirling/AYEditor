#include "AYEditorSession.h"

#include "AYEditorHeapDebug.h"
#include "AYSplitterHandle.h"
#include "AYButton.h"
#include "AYGameLoop.h"
#include "AYTextLabel.h"
#include "AYWidget.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

namespace ayt::editor {

EditorSession::EditorSession()
    : _gameView(ayt::game::GameLoop::instance(), _playRuntime) {
}

EditorSession::~EditorSession() {
    shutdown();
}

bool EditorSession::initialize(const EditorSessionDesc& desc) {
    AY_EDITOR_TRACE("initialize: begin");
    _hostWindow = desc.hostWindow;
    _layoutPath = desc.layoutPath;
    _playRuntime.setHostWindow(_hostWindow);
    // ED-02: forward the imported character (if any) to the
    // Play-runtime. Empty / invalid = cube fallback at startPlay.
    _playRuntime.setImportedCharacter(desc.importedCharacter);
    AY_EDITOR_HEAP_CHECK("session_after_set_host");

    _ui.initialize(desc.uiBackend);
    AY_EDITOR_TRACE("initialize: ui backend set");
    _gameView.setModeChangedCallback([this](EditorMode mode) { onModeChanged(mode); });

    if (_hostWindow != nullptr) {
        RECT clientRect{};
        if (GetClientRect(_hostWindow, &clientRect) != 0) {
            const float width  = static_cast<float>(clientRect.right - clientRect.left);
            const float height = static_cast<float>(clientRect.bottom - clientRect.top);
            if (width > 0.0f && height > 0.0f) {
                _ui.setClientSize(width, height);
                _playRuntime.setClientSize(static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
            }
        }
    }

    AY_EDITOR_TRACE("initialize: loading layout");
    if (!_layoutPath.empty()) {
        if (!_ui.loadLayout(_layoutPath)) {
            std::fprintf(stderr, "[EditorSession] loadLayout failed: %s\n", _layoutPath.c_str());
            return false;
        }
    }
    AY_EDITOR_HEAP_CHECK("session_after_load_layout");
    AY_EDITOR_TRACE("initialize: layout loaded");

    bindToolbar();
    AY_EDITOR_HEAP_CHECK("session_after_bind_toolbar");
    AY_EDITOR_TRACE("initialize: toolbar bound");

    setModeLabel(L"EDIT");
    AY_EDITOR_HEAP_CHECK("session_after_set_mode_label");

    syncViewport();
    AY_EDITOR_HEAP_CHECK("session_after_sync_viewport");
    AY_EDITOR_TRACE("initialize: done");
    return true;
}

bool EditorSession::initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath) {
    EditorSessionDesc desc;
    desc.uiBackend = backend;
    desc.layoutPath = layoutPath;
    return initialize(desc);
}

void EditorSession::shutdown() {
    if (_shutdown) {
        return;
    }
    _shutdown = true;

    _gameView.setModeChangedCallback({});
    _repaintCallback = nullptr;
    _gameView.setMode(EditorMode::Edit);
    _ui.shutdown();
    _playRuntime.shutdownEngine();
    _layoutPath.clear();
    _hostWindow = nullptr;
}

void EditorSession::setClientSize(float width, float height) {
    _ui.setClientSize(width, height);
    _playRuntime.setClientSize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
    _ui.layout();
    syncViewport();
}

void EditorSession::update(float dt) {
    _ui.update(dt);

    if (_gameView.mode() == EditorMode::Play) {
        _playRuntime.tick();
    } else if (_gameView.mode() == EditorMode::Paused) {
        // Keep last rendered frame visible; stepOnce drives simulation separately.
    }
}

void EditorSession::render() {
    render(false);
}

void EditorSession::render(bool skipViewportPanel) {
    ayt::ui::Widget* viewport = nullptr;
    bool viewportWasVisible = true;
    if (skipViewportPanel) {
        viewport = _ui.findById("panel_viewport");
        if (viewport != nullptr) {
            viewportWasVisible = viewport->isVisible();
            viewport->setVisible(false);
        }
    }

    _ui.render();

    if (viewport != nullptr) {
        viewport->setVisible(viewportWasVisible);
    }
}

bool EditorSession::shouldCompositeViewport() const {
    const EditorMode mode = _gameView.mode();
    return (mode == EditorMode::Play || mode == EditorMode::Paused)
           && _playRuntime.isPresentationReady();
}

bool EditorSession::ensurePresentationReady() {
    return _playRuntime.ensurePresentationReady();
}

bool EditorSession::getViewportBounds(ayt::math::FRectangle& outBounds) const {
    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return false;
    }
    outBounds = viewport->getWorldBounds();
    return true;
}

bool EditorSession::isSplitHandlePoint(float x, float y) const {
    ayt::ui::Widget* mainRow = _ui.findById("main_row");
    if (mainRow == nullptr) {
        return false;
    }

    ayt::ui::Widget* hit = mainRow->hitTest(ayt::math::FVector2(x, y));
    return dynamic_cast<const ayt::ui::SplitterHandle*>(hit) != nullptr;
}

bool EditorSession::isChromePoint(float x, float y) const {
    if (_gameView.mode() == EditorMode::Edit) {
        return true;
    }

    if (_ui.isCapturing()) {
        return true;
    }

    if (isSplitHandlePoint(x, y)) {
        return true;
    }

    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)) {
        return true;
    }

    return x < viewport.minX || x >= viewport.maxX
        || y < viewport.minY || y >= viewport.maxY;
}

bool EditorSession::onMouseMove(float x, float y) {
    if (_ui.isCapturing()) {
        return _ui.onMouseMove(x, y);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        return false;
    }

    return _ui.onMouseMove(x, y);
}

bool EditorSession::onMouseButtonDown(float x, float y, int button) {
    if (_ui.isCapturing()) {
        return _ui.onMouseButtonDown(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        return false;
    }

    return _ui.onMouseButtonDown(x, y, button);
}

bool EditorSession::onMouseButtonUp(float x, float y, int button) {
    if (_ui.isCapturing()) {
        return _ui.onMouseButtonUp(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        return false;
    }

    return _ui.onMouseButtonUp(x, y, button);
}

void EditorSession::onMouseLeave() {
    _ui.onMouseLeave();
}

bool EditorSession::isUiHoverInteractive() const {
    return _ui.isHoverInteractive();
}

ayt::ui::UiCursorHint EditorSession::getUiCursorHint() const {
    return _ui.getCursorHint();
}

void EditorSession::setRepaintCallback(RepaintCallback callback) {
    _repaintCallback = std::move(callback);
}

void EditorSession::bindToolbar() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_play", [this]() { _gameView.setMode(EditorMode::Play); });
    bindButton("btn_pause", [this]() { _gameView.setMode(EditorMode::Paused); });
    bindButton("btn_stop", [this]() { _gameView.setMode(EditorMode::Edit); });
    bindButton("btn_step", [this]() {
        _gameView.stepOnce();
        if (_repaintCallback) {
            _repaintCallback();
        }
    });

    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setBackgroundColor(ayt::math::FVector4(0.10f, 0.10f, 0.11f, 1.0f));
        }
    }
}

void EditorSession::setModeLabel(const std::wstring& text) {
    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

void EditorSession::onModeChanged(EditorMode mode) {
    _ui.cancelCapture();

    switch (mode) {
    case EditorMode::Edit:
        setModeLabel(L"EDIT");
        break;
    case EditorMode::Play:
        setModeLabel(L"PLAY");
        break;
    case EditorMode::Paused:
        setModeLabel(L"PAUSED");
        break;
    }

    syncViewport();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::syncViewportIfChanged() {
    if (_hostWindow == nullptr) {
        return;
    }

    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return;
    }

    const ayt::math::FRectangle bounds = viewport->getWorldBounds();
    if (_viewportBoundsCached
        && bounds.minX == _cachedViewportBounds.minX
        && bounds.minY == _cachedViewportBounds.minY
        && bounds.maxX == _cachedViewportBounds.maxX
        && bounds.maxY == _cachedViewportBounds.maxY) {
        return;
    }

    _cachedViewportBounds = bounds;
    _viewportBoundsCached = true;
    _playRuntime.syncViewportRect(bounds);
}

void EditorSession::syncViewport() {
    _viewportBoundsCached = false;
    syncViewportIfChanged();
}

} // namespace ayt::editor
