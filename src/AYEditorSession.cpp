#include "AYEditorSession.h"

#include "AYButton.h"
#include "AYGameLoop.h"
#include "AYTextLabel.h"
#include "AYWidget.h"

namespace ayt::editor {

EditorSession::EditorSession()
    : _gameView(ayt::game::GameLoop::instance(), _playRuntime) {
}

EditorSession::~EditorSession() {
    shutdown();
}

bool EditorSession::initialize(const EditorSessionDesc& desc) {
    _hostWindow = desc.hostWindow;
    _layoutPath = desc.layoutPath;
    _playRuntime.setHostWindow(_hostWindow);
    _playRuntime.setWindowManager(desc.windowManager);

    _ui.initialize(desc.uiBackend);
    _gameView.setModeChangedCallback([this](EditorMode mode) { onModeChanged(mode); });

    if (!_ui.loadLayout(_layoutPath)) {
        return false;
    }

    bindToolbar();
    setModeLabel(L"EDIT");
    syncViewport();
    return true;
}

bool EditorSession::initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath) {
    EditorSessionDesc desc;
    desc.uiBackend = backend;
    desc.layoutPath = layoutPath;
    return initialize(desc);
}

void EditorSession::shutdown() {
    _gameView.setMode(EditorMode::Edit);
    _playRuntime.shutdownEngine();
    _ui.shutdown();
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
           && _playRuntime.isEngineInitialized();
}

bool EditorSession::getViewportBounds(ayt::math::FRectangle& outBounds) const {
    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return false;
    }
    outBounds = viewport->getWorldBounds();
    return true;
}

bool EditorSession::isChromePoint(float x, float y) const {
    if (_gameView.mode() == EditorMode::Edit) {
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
    if (!isChromePoint(x, y)) {
        return false;
    }
    return _ui.onMouseMove(x, y);
}

bool EditorSession::onMouseButtonDown(float x, float y, int button) {
    if (!isChromePoint(x, y)) {
        return false;
    }
    return _ui.onMouseButtonDown(x, y, button);
}

bool EditorSession::onMouseButtonUp(float x, float y, int button) {
    if (!isChromePoint(x, y)) {
        return false;
    }
    return _ui.onMouseButtonUp(x, y, button);
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
}

void EditorSession::setModeLabel(const std::wstring& text) {
    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

void EditorSession::onModeChanged(EditorMode mode) {
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

void EditorSession::syncViewport() {
    if (_hostWindow == nullptr) {
        return;
    }

    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return;
    }

    _playRuntime.syncViewportRect(viewport->getWorldBounds());
}

} // namespace ayt::editor
