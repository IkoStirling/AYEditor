#pragma once

#include "AYEditor/EditorWorkspace.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::ui {
class DockArea;
class DockCard;
class UIManager;
}

namespace ayt::editor {

struct EditorDockViewOptions {
    std::string cardId;
    float headerHeight = 20.0f;
    bool closable = true;
};

struct EditorDockOpenResult {
    EditorOpenResult document;
    ayt::ui::DockCard* card = nullptr;
    std::string error;

    explicit operator bool() const noexcept {
        return static_cast<bool>(document) && card != nullptr;
    }
};

struct EditorHostedView {
    std::string documentId;
    std::string editorId;
    std::string cardId;
    EditorDockSlot dockSlot = EditorDockSlot::Center;
    std::shared_ptr<IEditorDocument> document;
    std::unique_ptr<IEditorView> view;
    ayt::ui::DockCard* card = nullptr;
    uint64_t presentedRevision = 0;
    bool presentedDirty = false;
    std::string presentedTitle;
    bool uiShutdownPrepared = false;
};

// AYEditor-owned bridge between model-only EditorWorkspace services and AYUI
// DockCards. It contains no resource/editor-specific semantics.
class EditorDockViewHost final : public IEditorHostServices {
public:
    using CloseActionProvider = std::function<EditorDocumentCloseAction(
        const EditorHostedView& hosted)>;

    EditorDockViewHost(EditorWorkspace& workspace,
                       ayt::ui::DockArea& dockArea,
                       IEditorHostServices& outerHost,
                       ayt::ui::UIManager* uiManager = nullptr);
    ~EditorDockViewHost() override;

    EditorDockViewHost(const EditorDockViewHost&) = delete;
    EditorDockViewHost& operator=(const EditorDockViewHost&) = delete;

    EditorDockOpenResult open(const EditorOpenRequest& request,
                              EditorDockViewOptions options = {});
    bool activate(const std::string& documentId);
    EditorCloseResult close(const std::string& documentId,
                            EditorDocumentCloseAction action);
    bool requestClose(ayt::ui::DockCard* card);

    EditorHostedView* find(const std::string& documentId) noexcept;
    const EditorHostedView* find(
        const std::string& documentId) const noexcept;
    EditorHostedView* findByCard(ayt::ui::DockCard* card) noexcept;
    const EditorHostedView* active() const noexcept;
    size_t count(const std::string& editorId = {}) const noexcept;

    void setCloseActionProvider(CloseActionProvider provider) {
        _closeActionProvider = std::move(provider);
    }
    void syncCommandTargetFromFocus();
    void refreshPresentations();
    void tick(float dt);

    bool routePointerDown(float physicalX, float physicalY, int button);
    bool routePointerMove(float physicalX, float physicalY);
    bool routePointerUp(float physicalX, float physicalY, int button);
    bool routeWheel(float physicalX, float physicalY, float deltaY);
    bool routeKeyDown(int keyCode);
    bool routeKeyUp(int keyCode);
    bool resolveCursorHint(float physicalX, float physicalY,
                           ayt::ui::UiCursorHint& hint) const;
    void releaseInputFocus();

    // Normal owner teardown closes hosted cards. A native child-window host
    // needs two phases: prepare keeps IEditorView alive while all AYUI trees
    // are destroyed, release clears the now-widget-free records afterward.
    void shutdown();
    void prepareForUiShutdown();
    void releaseAfterUiShutdown();

    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override {
        return _outerHost.projectRoot();
    }
    ayt::ui::UIManager* uiManager() noexcept override {
        return _uiManager;
    }
    std::string chooseImageFile() override {
        return _outerHost.chooseImageFile();
    }
    EditorAuthoringImage loadAuthoringImage(
        const std::string& path, std::string* error = nullptr) override {
        return _outerHost.loadAuthoringImage(path, error);
    }
    void requestRepaint() override;
    void setStatusText(const std::wstring& text) override {
        _outerHost.setStatusText(text);
    }

private:
    void onDocumentEvent(const EditorDocumentEvent& event);
    void activateHosted(EditorHostedView& hosted, bool revealCard);
    void removeHostedView(const std::string& documentId,
                          bool destroyCard);
    void closeDocumentsForShutdown();
    void prepareHostedUiShutdown(EditorHostedView& hosted);
    EditorHostedView* inputHostedAt(float logicalX, float logicalY) noexcept;
    EditorHostedView* inputFocusedHosted() noexcept;
    static std::string makeCardId(const std::string& documentId);

    EditorWorkspace& _workspace;
    ayt::ui::DockArea& _dockArea;
    IEditorHostServices& _outerHost;
    ayt::ui::UIManager* _uiManager = nullptr;
    std::vector<EditorHostedView> _hosted;
    std::string _activeDocumentId;
    std::string _inputDocumentId;
    CloseActionProvider _closeActionProvider;
    EditorDocumentManager::ListenerId _documentListener = 0;
    std::string _closingDocumentId;
    bool _preparedForUiShutdown = false;
    bool _releasedAfterUiShutdown = false;
};

} // namespace ayt::editor
