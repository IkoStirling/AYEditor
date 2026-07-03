#pragma once

#include "AYEditorGameView.h"
#include "AYEditorPlayRuntime.h"
#include "AYUIManager.h"

#include "AYMathTypes.h"

#include <functional>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace ayt::device {
class WindowManager;
}

namespace ayt::editor {

struct EditorSessionDesc {
    ayt::ui::IRenderBackend* uiBackend = nullptr;
    std::string layoutPath;
    HWND hostWindow = nullptr;
    ayt::device::WindowManager* windowManager = nullptr;
};

class EditorSession {
public:
    EditorSession();
    ~EditorSession();

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    bool initialize(const EditorSessionDesc& desc);
    bool initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath);
    void shutdown();

    void setClientSize(float width, float height);
    void update(float dt);
    void render();
    void render(bool skipViewportPanel);

    bool shouldCompositeViewport() const;
    bool getViewportBounds(ayt::math::FRectangle& outBounds) const;

    bool onMouseMove(float x, float y);
    bool onMouseButtonDown(float x, float y, int button);
    bool onMouseButtonUp(float x, float y, int button);

    using RepaintCallback = std::function<void()>;
    void setRepaintCallback(RepaintCallback callback);

    EditorGameView& gameView() { return _gameView; }
    const EditorGameView& gameView() const { return _gameView; }
    ayt::ui::UIManager& ui() { return _ui; }
    const ayt::ui::UIManager& ui() const { return _ui; }

private:
    void bindToolbar();
    void setModeLabel(const std::wstring& text);
    void onModeChanged(EditorMode mode);
    void syncViewport();
    bool isChromePoint(float x, float y) const;

    EditorPlayRuntime _playRuntime;
    EditorGameView _gameView;
    ayt::ui::UIManager _ui;
    HWND _hostWindow = nullptr;
    std::string _layoutPath;
    RepaintCallback _repaintCallback;
};

} // namespace ayt::editor
