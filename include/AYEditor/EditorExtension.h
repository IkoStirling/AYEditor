#pragma once

#include "AYEditor/EditorVersion.h"

#include <cstdint>
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

class EditorCommandRouter;
class EditorDocumentManager;
class EditorSelectionContext;
class EditorWorkspace;

enum class EditorSurfaceKind : uint8_t {
    Document,
    ToolPanel,
    ModalWorkflow,
};

enum class EditorOpenPolicy : uint8_t {
    PerResource,
    Singleton,
    Multiple,
};

enum class EditorDockSlot : uint8_t {
    Center,
    Left,
    Right,
    Bottom,
};

struct EditorOpenRequest {
    std::string resourcePath;
    std::string resourceKey;
    std::string assetType;
    std::string preferredEditorId;
    // Presentation-only path. Kept last so existing aggregate initialization
    // and the stable offsets of routing fields remain source/binary friendly.
    std::string displayPath;
};

class IEditorDocument {
public:
    virtual ~IEditorDocument() = default;

    virtual const std::string& typeId() const noexcept = 0;
    virtual const std::string& path() const noexcept = 0;
    virtual const std::string& title() const noexcept = 0;
    virtual bool isDirty() const noexcept = 0;
    virtual uint64_t revision() const noexcept = 0;

    virtual bool save(std::string* error = nullptr) = 0;

    virtual bool canSaveAs() const noexcept { return false; }
    virtual bool saveAs(const std::string& /*path*/,
                        std::string* error = nullptr) {
        if (error != nullptr) *error = "Save As is not supported.";
        return false;
    }

    virtual bool canReload() const noexcept { return false; }
    virtual bool reload(std::string* error = nullptr) {
        if (error != nullptr) *error = "Reload is not supported.";
        return false;
    }

    // Serialize the current in-memory document to a recovery-only path
    // without changing its title, source path, dirty flag, or undo history.
    virtual bool writeRecoveryCopy(const std::string& /*path*/,
                                   std::string* error = nullptr) const {
        if (error != nullptr) *error = "Crash recovery is not supported.";
        return false;
    }
};

class IEditorCommandTarget {
public:
    virtual ~IEditorCommandTarget() = default;
    virtual bool handlesCommand(const std::string& commandId) const = 0;
    virtual bool canExecuteCommand(const std::string& commandId) const = 0;
    virtual bool executeCommand(const std::string& commandId) = 0;
};

// AYEditor-owned services exposed to a registered editor view. Module editor
// cores remain independent from this interface; only their AYEditor adapter
// consumes it.
class IEditorHostServices {
public:
    virtual ~IEditorHostServices() = default;
    virtual EditorWorkspace& workspace() noexcept = 0;
    virtual const std::string& projectRoot() const noexcept = 0;
    virtual ayt::ui::UIManager* uiManager() noexcept { return nullptr; }
    virtual void requestRepaint() = 0;
    virtual void setStatusText(const std::wstring& text) = 0;
};

// Optional logical-coordinate input surface for complex visual editors. The
// host invokes it before normal AYUI dispatch, matching standalone editor
// hosts that need canvas manipulation without teaching AYUI business meaning.
class IEditorViewInputTarget {
public:
    virtual ~IEditorViewInputTarget() = default;
    virtual bool onPointerDown(float x, float y, int button) = 0;
    virtual bool onPointerMove(float x, float y) = 0;
    virtual bool onPointerUp(float x, float y, int button) = 0;
    virtual bool onWheel(float x, float y, float deltaY) = 0;
    virtual bool onKeyDown(int keyCode) = 0;
    virtual void onKeyUp(int keyCode) = 0;
    virtual bool hasPointerCapture() const noexcept = 0;
    virtual ayt::ui::UiCursorHint cursorHint(float x, float y) const = 0;
};

class IEditorView {
public:
    virtual ~IEditorView() = default;

    // The view owns its content tree until releaseRootWidget transfers that
    // tree to an AYUI host such as DockCard. The view object must then outlive
    // the hosted tree because widget callbacks may still target it.
    virtual ayt::ui::Widget* rootWidget() noexcept = 0;
    virtual ayt::ui::Widget* releaseRootWidget() noexcept = 0;
    virtual IEditorCommandTarget* commandTarget() noexcept { return nullptr; }
    virtual IEditorViewInputTarget* inputTarget() noexcept { return nullptr; }
    virtual EditorSelectionContext* selectionContext() noexcept {
        return nullptr;
    }

    virtual void onActivated() {}
    virtual void onDeactivated() {}
    // Called while the hosted widget tree is still alive. Views that keep
    // widget callbacks/raw aliases detach them here before DockCard teardown.
    virtual void prepareForUiShutdown() {}
    virtual void tick(float /*dt*/) {}
    virtual bool wantsBackgroundTick() const noexcept { return false; }
};

using EditorDocumentFactory = std::function<std::shared_ptr<IEditorDocument>(
    const EditorOpenRequest& request, std::string& error)>;
using EditorViewFactory = std::function<std::unique_ptr<IEditorView>(
    const std::shared_ptr<IEditorDocument>& document,
    IEditorHostServices& host)>;

struct EditorDescriptor {
    std::string id;
    std::wstring displayName;
    std::string iconPath;
    EditorSurfaceKind surfaceKind = EditorSurfaceKind::Document;
    EditorOpenPolicy openPolicy = EditorOpenPolicy::PerResource;
    EditorDockSlot defaultDockSlot = EditorDockSlot::Center;
    int priority = 0;
    std::vector<std::string> extensions;
    std::vector<std::string> assetTypes;
    EditorDocumentFactory createDocument;
    EditorViewFactory createView;
};

} // namespace ayt::editor
