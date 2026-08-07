#pragma once

#include "AYEditorGameView.h"
#include "AYEditorPlayRuntime.h"
#include "AYEditorFreecam.h"
#include "AYImportedCharacterMapper.h"
#include "AYImportDialog.h"
#include "AYImporter.h"
#include "AYInspectorOverrides.h"
#include "AYUIManager.h"
#include "AYDockArea.h"
#include "AYDockCard.h"
#include "EditorChildWindowManager.h"

#include "aymath/MathTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct HWND__;
using HWND = HWND__*;

namespace ayt::device { class WindowManager; }

// v0.3 PR-4 — forward decl Scene（design §4.2.x）
// Scene 完整定义在 .cpp 引入（AYScene.h），避免把 AYScene 完整 lib 暴露到
// 任何 include AYEditorSession.h 的 TU；与 PR-3 caller 持 _edit ownership
// 路径对齐（AYSceneManager.h:69-76；SM 绝对不持 ownership）。
// 决策 1a: caller 持 ownership（PR-4）
// 决策 2a: 不接 EditorPlayRuntime 私有通路（PR-4）
// 决策 3a: EditorMode vs SceneMode 分离（PR-4；3 态 vs 2 态）
// **文件作用域 forward decl**：必须在 `namespace ayt::editor {` 之外声明，
// 否则会嵌套在 `ayt::editor::ayt::scene::Scene`，导致
// `std::unique_ptr<ayt::scene::Scene>` 类型校验失败。
namespace ayt::scene { class Scene; }

// v0.3+ PR-5 — forward decl TreeView（design §4.3.y）
// TreeView 完整定义在 .cpp 引入（AYTreeView.h），避免把 AYUI 全头暴露到
// 任何 include AYEditorSession.h 的 TU。**文件作用域** 同 PR-4 landmine。
namespace ayt::ui { class TreeView; }

namespace ayt::editor {

// `ImportedCharacter` is defined in `AYEditorPlayRuntime.h` (included
// above). The editor session forwards it straight through to the
// Play runtime; no need to redeclare.

struct EditorSessionDesc {
    ayt::ui::IRenderBackend* uiBackend = nullptr;
    std::string layoutPath;
    HWND hostWindow = nullptr;
    ImportedCharacter importedCharacter;  // empty = fall back to cube

    // Dual-process net demo: `--net-client` auto-enters Play and connects.
    bool netClientMode = false;
    std::string netConnectHost = "127.0.0.1";

    // D5+.5 (2026-07-26): optional child-window manager wiring.
    // Both fields are nullable (no children requested by default).
    // When `childWindowManager` is non-null + `childWindowConfigPath`
    // is non-empty, EditorSession parses the JSON + opens each entry
    // through the manager after primary UIManager is initialized.
    ayt::device::WindowManager* childWindowManager = nullptr;
    std::string childWindowConfigPath;
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

    bool onMouseMove(float x, float y);
    bool onMouseButtonDown(float x, float y, int button);
    bool onMouseButtonUp(float x, float y, int button);
    void onMouseLeave();

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
    ayt::ui::UIManager& ui() { return _ui; }
    const ayt::ui::UIManager& ui() const { return _ui; }

    // D5.5 (2026-07-26): accessor for the optional child-window manager
    // so the promote-callback injection (wirePromoteCallback) can route
    // detachToOwnWindow into it. Returns nullptr when no manager is
    // active (EditorSessionDesc::childWindowManager was null).
    EditorChildWindowManager* childWindows() { return _childWindows.get(); }

private:
    void bindToolbar();
    void bindMenuBar();
    void bindTransportBar();
    void bindNetworkPanelStub();
    void bindRenderSettingsPanel();
    void applyRenderSettingsFromPanel();
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

    // Viewport LMB click (no drag) → select primary Play entity.
    void selectPlayEntityFromViewport();

    // ED-03: commit the picked paths to the live character
    // (and to the runtime's pending-overrides buffer so a
    // future spawn re-applies them). Bound to [Apply].
    void applyInspectorOverrides();

    // ED-03: clear pending overrides (= reset path picks back
    // to the ImportedCharacter-derived defaults). Bound to
    // [Reset]. Refreshes inspector labels after.
    void resetInspectorOverrides();

    // ED-03: pending paths picked via Pick Skel / Pick Anim
    // buttons. Both empty = nothing to Apply. Apply wraps
    // these into an EntityInspectorOverrides and forwards
    // through selectCharacter / _playRuntime.
    void pickInspectorSkeleton();
    void pickInspectorAnimation();

    // ED-03: thin setter for the inspector's staged override
    // fields, called from pickInspector{Skel,Anim} after the
    // Win32 dialog returns a path.
    void setInspectorSkeletonPath(const std::string& path);
    void setInspectorAnimationPath(const std::string& path);

    // ED-03: shared work for [Apply] - build the override
    // struct from staged fields and forward.
    void commitInspectorOverrides(const EntityInspectorOverrides& ov);

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
    //   Edit        → host->scenes()->edit()->world()（v1 永远空，
    //                 因为没有任何路径往 Edit World 建 entity ——
    //                 见 AYScene.cpp:44 私有 World 实例 vs
    //                 AYEditorPlayRuntime.cpp:454/1224/1632 singleton spawn）
    //   Play/Paused → ayt::entity::World::instance()（EditorPlayRuntime
    //                 实际 spawn 目标）
    // onOutlinerSelectionChanged: 行点击 → Inspector。flatIndex 0 = 合成
    //   scene root（不可选）；>0 映射 _outlinerEntityIds[flatIndex - 1]。
    //   **Landmine B**：**不得**在此同步调 refreshOutliner()/_ui.layout()，
    //   TreeView::rebuildNodes() 会 delete 当前正在派发事件的 TreeNode
    //   （AYTreeView.cpp:80-85 + :194），UIManager::onMouseButtonUp:1339
    //   随后 deref 已释放的 _hoverWidget → UAF。改置 _outlinerRefreshPending。
    void bindOutlinerPanel();
    void refreshOutliner();
    void onOutlinerSelectionChanged(int flatIndex);
    void syncViewport();
    bool isChromePoint(float x, float y) const;
    bool isSplitHandlePoint(float x, float y) const;
    // Freecam WASD uses GetAsyncKeyState (global). Gate on host HWND
    // foreground + viewport hover (or active LMB look).
    bool viewportAcceptsGameInput() const;
    void clearSplitterHovers();
    void syncSplitterRevealToMouse();

    EditorPlayRuntime _playRuntime;
    EditorGameView _gameView;
    ayt::ui::UIManager _ui;

    // v0.3 PR-4 — Editor 持 Edit Scene（design §4.2.x）
    // std::unique_ptr<ayt::scene::Scene>（SceneMode::Edit），与 EditorSession 同寿。
    // initialize() 末尾走 host->scenes()->setEdit() + setCurrent() 注入；
    // shutdown() 末尾 reverse（setEdit(nullptr) + setCurrent(nullptr) + reset）。
    // SM 不持 _editScene ownership（PR-3 caller 持 _edit 路径对齐）。
    std::unique_ptr<ayt::scene::Scene> _editScene;

    // D5+.5 (2026-07-26): optional child-window manager. Late-bound
    // via make_unique in initialize() when desc.childWindowManager
    // is non-null. Reset BEFORE _ui.shutdown() per K-INV-D5-6:
    // ~EditorChildWindowManager destroys child HWNDs while primary
    // UI is still alive (its render path can be called with active
    // pointer in primary, never nullptr).
    std::unique_ptr<EditorChildWindowManager> _childWindows;

    HWND _hostWindow = nullptr;
    std::string _layoutPath;
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

    // ED-03: staged Inspector pick state. Populated by
    // pickInspector{Skel,Anim} via Win32 dialogs; consumed by
    // applyInspectorOverrides to build the EntityInspector
    // Overrides struct. Both empty = nothing staged = [Apply]
    // is a no-op. Reset clears them.
    std::string _inspectorSkelPick;
    std::string _inspectorAnimPick;

    // Viewport click cycles Character ↔ opaque cube so the Inspector
    // visibly changes (full ray-pick deferred).
    bool _inspectorPreferCube = false;
    uint32_t _viewportClickCount = 0;
    bool _netClientAutoPlay = false;

    ayt::ui::DockArea* _mainDock = nullptr;

    bool _panelRenderVisible = true;
    bool _panelInspectorVisible = true;
    bool _panelNetworkVisible = false;
    bool _panelOutlinerVisible = true;  // v0.3+ PR-5

    // v0.3+ PR-5 — Outliner state。
    // _outliner: 非持有（UIManager/DockCard 持树 ownership）；shutdown()
    //   在 _ui.shutdown() 前置 nullptr（Landmine E）。
    // _outlinerEntityIds: flatIndex-1 → Entity id（**id 而非 Entity***：
    //   endPlay / World teardown 后裸指针会 dangle；走 World::findEntity
    //   （AYWorld.h:42）重解析，miss = 已销毁 → 自动降级 Landmine F）。
    // _outlinerSelectedEntityId: 0 = 无 Hierarchy 选择（Inspector 退回
    //   PR-4 的 character/cube 二选一路径）。
    // _outlinerRefreshPending: 延迟重建标志；update(dt) 内消费（Landmine B）。
    ayt::ui::TreeView*    _outliner = nullptr;
    std::vector<uint32_t> _outlinerEntityIds;
    uint32_t              _outlinerSelectedEntityId = 0;
    bool                  _outlinerRefreshPending = false;

    // PR-5 (v0.1.2 LM-2): Play/Paused 时锁 Inspector 写路径。
    // onModeChanged 切 mode 时同步切换。Inspector 4 button click handler
    // (pickInspector{Skel,Anim} / applyInspectorOverrides /
    // resetInspectorOverrides) 入口守卫 `if (!allowInspectorEdit()) return;`。
    // 视觉提示走 `inspector_hint` TextLabel 文案切换 ("No selection" /
    // "Locked during Play") — AYUI Button 没有 setEnabled 接口
    // (D:/Projects/AYRuntime/AYUI/Controls/AYButton.h), 故仅做 click 早返。
    bool _allowInspectorEdit = true;

    // PR-5 LM-2 helper — Inspector 4 click handler + apply/commit 入口守卫。
    bool allowInspectorEdit() const noexcept { return _allowInspectorEdit; }
};

} // namespace ayt::editor
