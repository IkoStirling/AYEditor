#pragma once

#include "AYEditor/EditorGameView.h"
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEditor/EditorWorldContext.h"
#include "AYEditor/EditorFreecam.h"
#include "AYEditor/EditorSceneDocument.h"
#include "AYEditor/EditorSelection.h"
#include "AYEditor/EditorCommandStack.h"
#include "AYEditor/EditorTransformGizmo.h"
#include "AYEditor/EditorPreferences.h"
#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/ImportedCharacterMapper.h"
#include "AYEditor/ImportDialog.h"
#include "AYEditor/Importer.h"
#include "AYEditor/InspectorOverrides.h"
#include "AYUI/UIManager.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYEditor/EditorChildWindowManager.h"

#include "AYMath/MathTypes.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct HWND__;
using HWND = HWND__*;

namespace ayt::device { class DeviceManager; class WindowManager; }

// v0.3 PR-4 — forward decl Scene（design §4.2.x）
// Scene 完整定义在 .cpp 引入（AYScene.h），避免把 AYScene 完整 lib 暴露到
// 任何 include AYEditor/EditorSession.h 的 TU；与 PR-3 caller 持 _edit ownership
// 路径对齐（AYScene/SceneManager.h:69-76；SM 绝对不持 ownership）。
// 决策 1a: caller 持 ownership（PR-4）
// 决策 2a: 不接 EditorPlayRuntime 私有通路（PR-4）
// 决策 3a: EditorMode vs SceneMode 分离（PR-4；3 态 vs 2 态）
// **文件作用域 forward decl**：必须在 `namespace ayt::editor {` 之外声明，
// 否则会嵌套在 `ayt::editor::ayt::scene::Scene`，导致
// `std::unique_ptr<ayt::scene::Scene>` 类型校验失败。
namespace ayt::scene { class Scene; }

// v0.3+ PR-5 — forward decl TreeView（design §4.3.y）
// TreeView 完整定义在 .cpp 引入（AYTreeView.h），避免把 AYUI 全头暴露到
// 任何 include AYEditor/EditorSession.h 的 TU。**文件作用域** 同 PR-4 landmine。
namespace ayt::ui {
class TreeView;
class TileView;
class TextInput;
class ComboBox;
class VBox;
class MenuItem;
class Button;
class Image;
class ModalDialog;
class CheckBox;
class ListView;
}
namespace ayt::audio { class AudioEditorSession; }
namespace ayt::audio { class AudioSubSystem; }

namespace ayt::editor {

class EditorWorkspace;
class EditorDockViewHost;
class IEditorHostServices;
class EditorUiLayoutController;
class EditorUiLayoutDocument;
class EditorAssetPreviewCache;
class EditorAssetImportQueue;
class EditorAssetTrash;
class EditorAssetOperations;
class EditorRecoveryStore;

// `ImportedCharacter` is defined in `AYEditor/EditorPlayRuntime.h` (included
// above). The editor session forwards it straight through to the
// Play runtime; no need to redeclare.

struct EditorSessionDesc {
    ayt::ui::IRenderBackend* uiBackend = nullptr;
    std::string layoutPath;
    // Open project root. Assets/ is the editable source tree and
    // .ayeditor_cache/assets/ is the generated/imported tree.
    std::string projectRoot;
    // Root of immutable installed/source engine assets. Child tools and the
    // play-runtime resolve their fixed inputs from this explicit mount.
    std::string engineAssetsRoot;
    // Optional SVG icon directory containing Tabler's outline/ and filled/
    // folders. Empty keeps the JSON text placeholders, which makes embedded
    // and headless hosts independent from editor-only visual assets.
    std::string iconRootPath;
    HWND hostWindow = nullptr;
    ImportedCharacter importedCharacter;  // empty = fall back to cube
    // Test/demo-only authoring content. False is the generic editor default;
    // AYEditorShell_Demo opts in explicitly at its composition root.
    bool editorTestSceneEnabled = false;

    // Dual-process net demo: `--net-client` auto-enters Play and connects.
    bool netClientMode = false;
    std::string netConnectHost = "127.0.0.1";

    // D5+.5 (2026-07-26): optional child-window manager wiring.
    // Both fields are nullable (no children requested by default).
    // When `childWindowManager` is non-null + `childWindowConfigPath`
    // is non-empty, EditorSession parses the JSON + opens each entry
    // through the manager after primary UIManager is initialized.
    ayt::device::WindowManager* childWindowManager = nullptr;
    ayt::device::DeviceManager* deviceManager = nullptr;
    std::string childWindowConfigPath;

    // Editor preference supplied by the shell host. The callback persists
    // user changes outside scene documents; empty keeps embedded/test hosts
    // fully in-memory.
    bool viewportOrientationAxisVisible = true;
    std::function<void(bool)> onViewportOrientationAxisVisibilityChanged;
    EditorPreferences preferences;
    std::function<void(const EditorPreferences&)> onPreferencesChanged;

    // Optional host-owned startup progress sink. EditorSession reports only
    // its own layout/binding portion as a normalized 0..1 range; EditorApp
    // maps that subrange into the process-wide splash progress.
    std::function<void(float, const wchar_t*)> onStartupProgress;

    // Optional host upload bridge for Content Browser raster previews.
    // Decode stays inside AYEditor and runs asynchronously; the actual GPU
    // handle is created/released by the host's concrete UI backend. Empty
    // callbacks keep Foundation/headless hosts free of renderer coupling.
    std::function<void*(std::uint16_t, std::uint16_t, const void*)>
        createAssetPreviewTexture;
    std::function<void(void*)> releaseAssetPreviewTexture;
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
    // Preferred host-facing update path. The input boundary must have been
    // polled before this call; GameLoop receives the same context in Play.
    void update(const ayt::game::HostedFrameContext& hostFrame);
    // Compatibility adapter for callers that only have a delta time.
    void update(float dt);
    void syncViewportIfChanged();
    void render();
    void render(bool skipViewportPanel);

    // AI-1 (2026-07-20): split render(bool) into populate + flush so
    // AYRenderer's RenderPass dispatch can own the UI submission
    // boundary (UIPass::execute flushes pending text). The
    // skipViewportPanel toggle is preserved across both calls via an
    // internal flag — populateFrame hides the viewport, flushFrame
    // restores it. render(bool) remains as a back-compat wrapper.
    void populateFrame(bool skipViewportPanel);
    void flushFrame();

    bool shouldCompositeViewport() const;
    bool ensurePresentationReady();
    bool getViewportBounds(ayt::math::FRectangle& outBounds) const;

    // `--net-client`: enter Play immediately after presentation bootstrap.
    void autoEnterNetClientPlay();
    // Demo animation preview: enter Play after a local animated character
    // has been imported and presentation bootstrap is complete.
    void autoEnterImportedAnimationPlay();

    bool onMouseMove(float x, float y);
    bool onMouseButtonDown(float x, float y, int button);
    bool onMouseButtonUp(float x, float y, int button);
    bool onMouseWheel(float x, float y, float deltaY);
    void onMouseLeave();
    bool onKeyDown(int keyCode);
    bool onKeyUp(int keyCode);
    void onWindowFocusChanged(bool focused);

    bool isUiHoverInteractive() const;
    ayt::ui::UiCursorHint getUiCursorHint() const;

    using RepaintCallback = std::function<void()>;
    void setRepaintCallback(RepaintCallback callback);

    EditorGameView& gameView() { return _gameView; }
    const EditorGameView& gameView() const { return _gameView; }
    // v0.4 PR-1: 暴露 play runtime 给单测（_gameView 路径下若需要直接
    // 调 startPlay/enterEdit/setNetPlayRole 等；不暴露给非测试 caller）。
    EditorPlayRuntime& playRuntime() { return _playRuntime; }
    const EditorPlayRuntime& playRuntime() const { return _playRuntime; }
    EditorWorldContext& worldContext() { return _worldContext; }
    const EditorWorldContext& worldContext() const { return _worldContext; }
    ayt::ui::UIManager& ui() { return _ui; }
    const ayt::ui::UIManager& ui() const { return _ui; }
    EditorSceneDocument* document() { return _document.get(); }
    const EditorSceneDocument* document() const { return _document.get(); }
    bool viewportOrientationAxisVisible() const noexcept {
        return _viewportOrientationAxisVisible;
    }
    uint32_t selectedEntityId() const noexcept { return _selection.entityId(); }
    EditorAssetId selectedAssetId() const noexcept { return _selectedAssetId; }
    const std::vector<EditorAssetId>& selectedAssetIds() const noexcept {
        return _selectedAssetIds;
    }
    EditorAssetDatabase& assetDatabase() noexcept { return _assetDatabase; }
    const EditorAssetDatabase& assetDatabase() const noexcept {
        return _assetDatabase;
    }
    EditorAssetImportQueue* assetImportQueue() noexcept {
        return _assetImportQueue.get();
    }
    // Host/test command surfaces used by the Content Browser actions.
    bool rescanAssetsNow();
    bool placeAssetInViewport(EditorAssetId assetId,
                              float physicalX, float physicalY);
    // Opens or focuses the source-backed Phoskia/Logia DockCard for an asset.
    // Returns false for non-DSL records or when the source cannot be read.
    bool openDslAsset(EditorAssetId assetId);
    bool openAsset(EditorAssetId assetId);
    std::size_t openDslDocumentCount() const noexcept;
    bool openUiLayoutEditor(const std::string& path = {});
    std::size_t openUiLayoutDocumentCount() const noexcept;
    bool createProjectAsset(EditorAssetType type);
    bool restoreLastDeletedAssets();
    bool runCurrentProject();
    bool autosaveNow();
    bool hasCrashRecovery() const noexcept;
    bool restoreCrashRecovery();
    bool renameSelectedAsset(const std::string& newFileName);
    bool moveSelectedAssets(const std::string& destinationLogicalFolder);
    bool copySelectedAssets(const std::string& destinationLogicalFolder);
    EditorWorkspace& workspace() noexcept;
    const EditorWorkspace& workspace() const noexcept;
    const EditorFreecam& freecam() const noexcept { return _freecam; }
    EditorTool activeTool() const noexcept { return _activeTool; }
    EditorPreferences currentPreferences() const;
    void savePreferencesNow();

    // D5.5 (2026-07-26): accessor for the optional child-window manager
    // so the promote-callback injection (wirePromoteCallback) can route
    // detachToOwnWindow into it. Returns nullptr when no manager is
    // active (EditorSessionDesc::childWindowManager was null).
    EditorChildWindowManager* childWindows() { return _childWindows.get(); }

private:
    void bindToolbar();
    void bindShellIcons(const std::string& iconRootPath);
    void bindMenuBar();
    void openAudioEditorWindow();
    void validateProjectContent();
    void syncAudioEditorLifetime();
    void syncUiDesignerLifetime();
    bool confirmUiDesignerClose();
    void releaseUiDesigner(bool closeDocument);
    void refreshUiDesignerTitle();
    bool openRegisteredTool(const std::string& editorId);
    void bindTransportBar();
    void bindNetworkPanelStub();
    void bindRenderSettingsPanel();
    void bindComponentBrowser();
    void refreshComponentBrowser();
    void addSelectedComponent();
    void removeSelectedComponent();
    void rebuildComponentPropertyEditor();
    void commitInspectorTextField(const std::string& componentType,
                                  const std::string& fieldName,
                                  int elementIndex,
                                  const std::wstring& text);
    void commitInspectorBoolField(const std::string& componentType,
                                  const std::string& fieldName,
                                  bool value);
    void commitInspectorColorField(const std::string& componentType,
                                   const std::string& fieldName,
                                   const ayt::math::FVector4& value);
    void refreshTransformInspector();
    void newSceneDocument();
    void openSceneDocument();
    void saveSceneDocument();
    void saveSceneDocumentAs();
    void afterDocumentReload();
    void createEmptyEntity();
    void deleteSelectedEntity();
    void applyRenderSettingsFromPanel();
    void applyPreferences(const EditorPreferences& preferences);
    EditorPreferences capturePreferences() const;
    void pollPreferences(float dtSeconds);
    void resetWorkspacePreferences();
    void setActiveTool(EditorTool tool);
    void setLocalTransformSpace(bool local);
    void toggleViewportProjection();
    void toggleViewportShading();
    void beginOrResumePlay();
    void pausePlay();
    void stopPlay();
    void setViewportOrientationAxisVisible(bool visible);
    void setDockCardVisible(const char* cardId, bool visible);
    void toggleDockCard(const char* cardId, bool& visibleFlag);
    void pushFreecamToRenderer();
    bool freecamActive() const;
    void requestHostClose();
    void requestHostMinimize();
    void requestHostMaximizeToggle();
    // Phase 2a: toolbar Import button handler. Opens the Win32
    // file picker, runs Importer::importFile + the G1 mapper,
    // and pushes the result into EditorPlayRuntime via
    // replaceImportedCharacter (which clears any existing entity
    // and respects the startPlay cube-fallback policy). Empty
    // path from the dialog = user cancelled = no-op.
    void importCharacterFromDialog();

    // ED-03: snapshot the current character entity's path
    // strings into the inspector labels, for use after a
    // hot-swap or pick-and-apply. No-op when no character is
    // currently spawned (writes "No selection" to the title
    // label).
    void refreshInspectorLabels();

    // ED-03: select the live character entity for inspection.
    // Bound to View → Select Character. If no character is
    // currently spawned, falls back to the procedural cube so
    // Inspector is never stuck on "No selection" while Play shows
    // something. Triggers a label refresh.
    void selectCharacter();

    // Viewport LMB click (no drag) → ray-pick the active Edit/Play world.
    void selectPlayEntityFromViewport();
    ayt::entity::Entity* pickEntityFromViewport(float x, float y);
    void applyViewportSelection(ayt::entity::World* world,
                                ayt::entity::Entity* entity);
    EditorGizmoHandle hitTestTransformGizmo(float x, float y);
    bool beginTransformGizmoDrag(EditorGizmoHandle handle,
                                 float x, float y);
    bool updateTransformGizmoDrag(float x, float y);
    void finishTransformGizmoDrag(bool commit);
    void updateTransformGizmoHover(float x, float y);
    void syncTransformGizmoToRenderer();
    void setSelectedEntity(ayt::entity::World* world,
                           ayt::entity::Entity* entity);
    void clearSelectedEntity(bool clearOutline = true,
                             bool clearAssetSelection = true);

    // D5.5 (2026-07-26): inject DockCard::setPromoteCallback into every
    // DockCard reachable through _ui.root(). The callback closes over
    // `this` and routes detachToOwnWindow() into the optional
    // EditorChildWindowManager — a floating card promoted to its own
    // HWND opens as a child window with the card's frame + id-derived
    // layoutPath. No-op when _childWindows is null.
    void wirePromoteCallback();

    void setModeLabel(const std::wstring& text);
    void setInspectorHint(const std::wstring& text);  // PR-5 (LM-2)
    void onModeChanged(EditorMode mode);
    void refreshUnsavedIndicator();  // v0.3 PR-4 (design §4.3.x 决策 5a)

    // v0.3+ PR-5 — Hierarchy / Outliner 面板（design §4.3.y）
    //
    // bindOutlinerPanel: 一次性 bind（selection callback + itemHeight）。
    //   在 initialize() 的 bindRenderSettingsPanel() 之后调一次。
    // refreshOutliner: 纯读重建 tree。**INV-4 锁**：只走
    //   world().getAllEntities() const + Entity::getId/getName，
    //   绝不 mutate Scene（Scene::_dirty 唯一写者是 clear/load/save，
    //   见 AYScene.h:118 注释）。
    // 决策 1b（mode-keyed World 源）：
    //   Edit        → EditorWorldContext Edit slot（无 fallback）
    //   Play/Paused → EditorWorldContext Play slot；Scene World 优先，
    //                 net-client / standalone 才使用显式 fallback World。
    // onOutlinerSelectionChanged: 行点击 → Inspector。flatIndex 0 = 合成
    //   scene root（不可选）；>0 映射 _outlinerEntityIds[flatIndex - 1]。
    //   **Landmine B**：**不得**在此同步调 refreshOutliner()/_ui.layout()，
    //   TreeView::rebuildNodes() 会 delete 当前正在派发事件的 TreeNode
    //   （AYTreeView.cpp:80-85 + :194），UIManager::onMouseButtonUp:1339
    //   随后 deref 已释放的 _hoverWidget → UAF。改置 _outlinerRefreshPending。
    void bindOutlinerPanel();
    void refreshOutliner();
    const ayt::entity::World* hierarchyWorld() const noexcept;
    ayt::entity::World* hierarchyWorldMutable() noexcept;
    void onOutlinerSelectionChanged(int flatIndex);
    void syncViewport();
    bool isViewportSurfacePoint(float x, float y) const;
    bool isChromePoint(float x, float y) const;
    bool isSplitHandlePoint(float x, float y) const;
    bool viewportRayDirection(float x, float y,
                              ayt::math::FVector3& outDirection) const;
    // Freecam reads AYDevice keyboard state. Gate on typed window focus +
    // viewport hover (or active LMB look).
    bool viewportAcceptsGameInput() const;
    void clearSplitterHovers();
    void syncSplitterRevealToMouse();

    // Project Content Browser. The database only indexes files; AYResource
    // remains the runtime loader. Tree/List callbacks defer destructive model
    // rebuilds to update() for the same event-lifetime reason as Outliner.
    void bindAssetBrowser();
    void refreshAssetBrowser();
    void rebuildAssetFolderMapping();
    void refreshAssetList();
    void selectAsset(EditorAssetId assetId);
    void selectAssetsFromIndices(const std::vector<int>& indices);
    void clearSelectedAsset();
    void refreshAssetInspector();
    void refreshVisibleAssetPreviews();
    void setInspectorAssetMode(bool assetMode);
    void importAssetFromDialog();
    void reloadSelectedAsset();
    void requestDeleteSelectedAssets();
    void requestRenameSelectedAsset();
    void requestRelocateSelectedAssets(bool copy);
    void showAssetOperationDialog(const std::wstring& title,
                                  const std::wstring& initialValue,
                                  bool rename, bool copy);
    void showCrashRecoveryDialog();
    bool restoreCrashRecovery(const std::vector<std::size_t>& indices);
    void showAssetTrashDialog();
    void showAssetHistoryDialog();
    void deleteSelectedAssetsConfirmed();
    void refreshAssetDeleteButton();
    void setAssetBrowserStatus(const std::wstring& text,
                               bool mirrorToConsole = false);

    // Declared before the runtime because EditorPlayRuntime borrows it.
    EditorWorldContext _worldContext;
    EditorPlayRuntime _playRuntime;
    EditorGameView _gameView;
    ayt::ui::UIManager _ui;

    // P0: document owns one stable Edit Scene for the whole session.
    // initialize() 末尾走 host->scenes()->setEdit() + setCurrent() 注入；
    // shutdown() 末尾 reverse（setEdit(nullptr) + setCurrent(nullptr) + reset）。
    // SceneManager borrows the document Scene; it never owns it.
    std::unique_ptr<EditorSceneDocument> _document;

    // D5+.5 (2026-07-26): optional child-window manager. Late-bound
    // via make_unique in initialize() when desc.childWindowManager
    // is non-null. Reset BEFORE _ui.shutdown() per K-INV-D5-6:
    // ~EditorChildWindowManager destroys child HWNDs while primary
    // UI is still alive (its render path can be called with active
    // pointer in primary, never nullptr).
    std::unique_ptr<EditorChildWindowManager> _childWindows;

    // UI Designer is a dedicated AYDevice-owned modeless tool window. Its
    // document remains in EditorWorkspace; only presentation is outside the
    // Scene DockArea.
    std::unique_ptr<EditorUiLayoutController> _uiDesigner;
    std::shared_ptr<EditorUiLayoutDocument> _uiDesignerDocument;
    std::string _uiDesignerDocumentId;
    EditorChildWindowManager::Handle _uiDesignerHandle = nullptr;

    // Audio Editor child (shared with AYAudio_AudioEditor).
    std::unique_ptr<ayt::audio::AudioEditorSession> _audioEditor;
    EditorChildWindowManager::Handle _audioEditorHandle = nullptr;

    HWND _hostWindow = nullptr;
    ayt::device::DeviceManager* _devices = nullptr;
    bool _hostFocused = false;
    std::string _layoutPath;
    std::string _engineAssetsRoot;
    RepaintCallback _repaintCallback;

    // AI-1: holds the viewport panel pointer across populateFrame +
    // flushFrame calls so the skipViewportPanel toggle can be
    // applied once at populate and reverted once at flush. nullptr
    // when no toggle is active (the normal composite path).
    ayt::ui::Widget* _panelViewportForFrame = nullptr;
    bool            _panelViewportWasVisibleForFrame = true;
    // Play composite: parent DockCard (`card_viewport`) must not paint
    // its opaque Panel fill over the PostProcess 3D blit. We toggle
    // Panel::setBackgroundEnabled rather than setVisible(false) so the
    // Center dock slot weight stays intact.
    ayt::ui::Panel* _cardViewportForFrame = nullptr;
    bool            _cardViewportHadBackgroundForFrame = true;
    ayt::math::FRectangle _cachedViewportBounds{};
    bool _viewportBoundsCached = false;
    bool _shutdown = false;
    bool _editWorldPrepared = false;

    // Last client-space mouse position observed by onMouseMove /
    // onMouseLeave. Used by syncSplitterRevealToMouse() so a missed
    // leave still un-reveals splitters on the next update tick.
    float _lastMouseX = 0.0f;
    float _lastMouseY = 0.0f;
    bool _hasLastMouse = false;

    // Play/Paused freecam (LMB drag look + WASD/QE). Edit mode inactive.
    EditorFreecam _freecam;
    // Click-vs-drag: LMB down on viewport arms a pending click; if the
    // cursor moves past slop we start freecam look instead of select.
    bool  _viewportLmbPending = false;
    bool  _viewportLmbDragged = false;
    float _viewportLmbX = 0.0f;
    float _viewportLmbY = 0.0f;

    EditorTool _activeTool = EditorTool::Select;
    bool _localTransformSpace = false;
    bool _orthographicView = false;
    bool _wireframeView = false;

    EditorTransformGizmo _transformGizmo;
    EditorGizmoHandle _gizmoHoverHandle = EditorGizmoHandle::None;
    uint16_t _gizmoDisabledHandleMask = 0u;
    uint32_t _gizmoDragEntityId = 0;
    ayt::entity::World* _gizmoDragWorld = nullptr;

    bool _netClientAutoPlay = false;

    ayt::ui::DockArea* _mainDock = nullptr;

    bool _panelRenderVisible = true;
    bool _panelInspectorVisible = true;
    bool _panelNetworkVisible = false;
    bool _panelOutlinerVisible = true;  // v0.3+ PR-5
    bool _panelConsoleVisible = true;
    bool _panelAssetsVisible = true;

    // v0.3+ PR-5 — Outliner state。
    // _outliner: 非持有（UIManager/DockCard 持树 ownership）；shutdown()
    //   在 _ui.shutdown() 前置 nullptr（Landmine E）。
    // _outlinerEntityIds: flatIndex-1 → Entity id（**id 而非 Entity***：
    //   endPlay / World teardown 后裸指针会 dangle；走 World::findEntity
    //   （AYEntity/World.h:42）重解析，miss = 已销毁 → 自动降级 Landmine F）。
    // Selection is shared by Hierarchy, viewport picking and Inspector.
    // _outlinerRefreshPending: 延迟重建标志；update(dt) 内消费（Landmine B）。
    ayt::ui::TreeView*    _outliner = nullptr;
    std::vector<uint32_t> _outlinerEntityIds;
    EditorSelection       _selection;
    ayt::entity::World*    _selectionWorld = nullptr;
    bool                  _outlinerRefreshPending = false;
    bool                  _outlinerRootExpanded = true;
    bool                  _updatingOutlinerSelection = false;

    EditorAssetDatabase _assetDatabase;
    EditorAssetTilePresenter _assetTilePresenter;
    ayt::ui::TileView* _assetTileView = nullptr;
    ayt::ui::TreeView* _assetTree = nullptr;
    ayt::ui::TextInput* _assetSearch = nullptr;
    ayt::ui::ComboBox* _assetTypeFilter = nullptr;
    std::vector<EditorAssetEntry> _assetEntries;
    std::vector<std::string> _assetFolderSourcePaths;
    std::vector<std::string> _assetFolderFlatPaths;
    std::vector<bool> _assetFolderSourceExpanded;
    std::string _assetCurrentFolder = "Assets";
    std::string _pendingAssetSelectionPath;
    EditorAssetId _selectedAssetId = 0;
    std::vector<EditorAssetId> _selectedAssetIds;
    EditorAssetId _pendingAssetOpenId = 0;
    std::unique_ptr<EditorAssetPreviewCache> _assetPreviewCache;
    std::unique_ptr<EditorAssetImportQueue> _assetImportQueue;
    int _assetImportProgressPercent = -1;
    std::unique_ptr<EditorAssetTrash> _assetTrash;
    std::unique_ptr<EditorAssetOperations> _assetOperations;
    std::unique_ptr<EditorRecoveryStore> _recoveryStore;
    float _autosaveCountdown = 30.0f;
    ayt::ui::Image* _assetInspectorPreview = nullptr;
    ayt::ui::Button* _assetDeleteButton = nullptr;
    std::unique_ptr<ayt::ui::ModalDialog> _assetDeleteDialog;
    std::unique_ptr<ayt::ui::ModalDialog> _assetOperationDialog;
    ayt::ui::TextInput* _assetOperationInput = nullptr;
    ayt::ui::ComboBox* _assetOperationFolderPicker = nullptr;
    std::unique_ptr<ayt::ui::ModalDialog> _recoveryDialog;
    std::vector<ayt::ui::CheckBox*> _recoveryChecks;
    std::unique_ptr<ayt::ui::ModalDialog> _assetTrashDialog;
    ayt::ui::ListView* _assetTrashList = nullptr;
    std::unique_ptr<ayt::ui::ModalDialog> _assetHistoryDialog;
    struct AssetDragData {
        EditorAssetId id = 0;
        EditorAssetType type = EditorAssetType::Unknown;
        std::string runtimePath;
    } _assetDragData;
    bool _assetBrowserRefreshPending = false;
    bool _updatingAssetSelection = false;
    // New document/command/selection service root. Existing Scene and panel
    // paths remain outside it until their individual migration phases.
    std::unique_ptr<EditorWorkspace> _workspace;
    std::unique_ptr<IEditorHostServices> _editorHostServices;
    std::unique_ptr<EditorDockViewHost> _dockViewHost;

    EditorCommandStack _commands;
    ayt::ui::ComboBox* _componentPicker = nullptr;
    std::vector<std::string> _componentPickerTypeNames;
    ayt::ui::ComboBox* _attachedComponentPicker = nullptr;
    ayt::ui::VBox* _componentPropertyBody = nullptr;
    std::vector<std::string> _attachedComponentTypeNames;
    std::string _inspectedComponentTypeName;
    bool _updatingComponentPicker = false;
    bool _updatingComponentPropertyCommit = false;
    bool _controlDown = false;
    ayt::ui::MenuItem* _undoMenuItem = nullptr;
    ayt::ui::MenuItem* _redoMenuItem = nullptr;
    ayt::ui::MenuItem* _restoreDeletedMenuItem = nullptr;
    ayt::ui::MenuItem* _restoreRecoveryMenuItem = nullptr;
    ayt::ui::MenuItem* _viewportOrientationAxisMenuItem = nullptr;
    // Session-persistent editor preference. Renderer itself defaults off so
    // non-editor hosts never receive the widget accidentally.
    bool _viewportOrientationAxisVisible = true;
    std::function<void(bool)> _onViewportOrientationAxisVisibilityChanged;

    EditorPreferences _preferences;
    EditorPreferences _lastObservedPreferences;
    std::function<void(const EditorPreferences&)> _onPreferencesChanged;
    float _preferencesPollCountdown = 0.0f;
    float _preferencesSaveCountdown = 0.0f;
    bool _preferencesDirty = false;
    bool _applyingPreferences = false;

};

} // namespace ayt::editor
