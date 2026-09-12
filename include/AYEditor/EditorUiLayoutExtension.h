#pragma once

#include "AYEditor/EditorExtensionRegistry.h"
#include "AYUI/LayoutEditor/LayoutEditorSession.h"
#include "AYUI/LayoutEditor/LayoutResourceCatalog.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::ui {
class UIManager;
class Widget;
enum class UiCursorHint;
}

namespace ayt::editor {

class EditorUiLayoutDocument;

inline constexpr const char* kEditorUiLayoutExtensionId =
    "ayeditor.ui-layout";

struct EditorUiLayoutExtensionConfig {
    std::function<std::string()> chromePath;
    std::function<std::string()> openPathPicker;
    std::function<std::string()> savePathPicker;
    std::function<std::string()> texturePathPicker;
    std::function<std::string()> themePathPicker;
    std::function<std::vector<ayt::ui::LayoutTextureResource>()>
        textureResourceProvider;
    // Project-level reusable components shared by every *.ui.json document.
    // Evaluated on attach so projects selected after editor construction use
    // the current asset root.
    std::function<std::string()> externalComponentLibraryPath;
    std::function<bool(const std::string&, std::string&)> openOwningFlowAction;
    std::function<bool(const std::string&, std::string&)>
        completeFlowSignalsAction;
    std::vector<ayt::ui::LayoutProjectRefactorKind> projectRefactorKinds;
    ayt::ui::LayoutEditorSession::ProjectRefactorAction projectRefactorAction;
};

// Shared UI-layout authoring controller used by both the AYEditor tool-window
// host and the optional generic workspace view. Keeping LayoutEditorSession
// behind this adapter prevents the two presentation routes from forking their
// save, dirty, input, or teardown behavior.
class EditorUiLayoutController {
public:
    using StateChanged = std::function<void()>;

    EditorUiLayoutController(
        std::shared_ptr<EditorUiLayoutDocument> document,
        EditorUiLayoutExtensionConfig config);
    ~EditorUiLayoutController();

    EditorUiLayoutController(const EditorUiLayoutController&) = delete;
    EditorUiLayoutController& operator=(const EditorUiLayoutController&) = delete;

    bool attach(ayt::ui::UIManager& ui, ayt::ui::Widget* chromeRoot = nullptr);
    void detach();
    bool isAttached() const noexcept;
    void pumpDeferred(float deltaSeconds = 0.0f);

    bool openDocument(const std::string& path);
    bool saveDocument(std::string* error = nullptr);
    void undo();
    void redo();

    bool onPointerDown(float x, float y, int button);
    bool onPointerMove(float x, float y);
    bool onPointerUp(float x, float y, int button);
    bool onWheel(float x, float y, float deltaY);
    bool onKeyDown(int keyCode);
    void onKeyUp(int keyCode);
    bool hasPointerCapture() const noexcept;
    ayt::ui::UiCursorHint cursorHint(float x, float y) const;

    void setStateChanged(StateChanged changed);
    const std::shared_ptr<EditorUiLayoutDocument>& document() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

EditorDescriptor makeEditorUiLayoutDescriptor(
    EditorUiLayoutExtensionConfig config);
bool registerEditorUiLayoutExtension(
    EditorExtensionRegistry& registry,
    EditorUiLayoutExtensionConfig config,
    std::string* error = nullptr);

} // namespace ayt::editor
