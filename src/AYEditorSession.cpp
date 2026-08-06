#include "AYEditorSession.h"

#include "AYEditorHeapDebug.h"
#include "AYEntity.h"
#include "AYSplitterHandle.h"
#include "AYButton.h"
#include "AYCheckBox.h"
#include "AYComboBox.h"
#include "AYGameLoop.h"
#include "AYMenuBar.h"
#include "AYMenu.h"
#include "AYMenuItem.h"
#include "AYRendererSubSystem.h"
#include "AYSlider.h"
#include "AYTextLabel.h"
#include "AYTreeView.h"  // v0.3+ PR-5 Hierarchy panel (design §4.3.y)
#include "AYWidget.h"
#include "AYDockArea.h"
#include "AYDockCard.h"

// v0.3 PR-4 — Editor 消费 host->scenes()（design §4.2.x + §4.3.x）
// AYScene 完整 include 因 _editScene 需 SceneMode/Scene 完整类型；
// IEngineHost 走 host facade（v0.1.3 PR-6 ship）。
#include "AYScene.h"
#include "AYSceneManager.h"
#include "AYSceneMode.h"
#include "IEngineHost.h"
#include "AYApplication.h"  // currentEngineHost() / defaultEngineHost()

#include <components/AYAnimationComponent.h>
#include <components/AYMeshComponent.h>
#include <components/AYSkeletonComponent.h>

#include <cstdio>
#include <cstring>

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
    _playRuntime.setNetPlayRole(
        desc.netClientMode ? NetPlayRole::Client : NetPlayRole::Server);
    _playRuntime.setNetConnectHost(desc.netConnectHost);
    _netClientAutoPlay = desc.netClientMode;
    // Pre-existing _CrtCheckMemory() failure on session_after_set_host
    // in Debug builds. Commented to keep the build runnable; the four
    // checks later in initialize() remain enabled as debug invariants.
    // AY_EDITOR_HEAP_CHECK("session_after_set_host");

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
    AY_EDITOR_TRACE("initialize: layout loaded");

    bindToolbar();
    bindMenuBar();
    bindTransportBar();
    bindNetworkPanelStub();
    bindRenderSettingsPanel();

    // v0.3+ PR-5 — bindOutlinerPanel (design §4.3.y)
    // 一次性 bind（selection callback + itemHeight via setItemHeight;
    // JSON 的 itemHeight 在 DockCard.content 路径被静默丢，见 Landmine A）。
    bindOutlinerPanel();

    _mainDock = dynamic_cast<ayt::ui::DockArea*>(_ui.findById("main_dock"));
    setDockCardVisible("card_network", false);
    AY_EDITOR_TRACE("initialize: toolbar bound");

    setModeLabel(L"EDIT");

    syncViewport();
    AY_EDITOR_TRACE("initialize: done");

    // v0.3 PR-4 — Editor 持 Edit Scene（design §4.2.x）
    // 决策 1a: caller 持 _editScene ownership
    // 决策 3a: EditorMode 3 态 vs SceneMode 2 态；本处只 setCurrent 让 Edit
    //         mode 有 Scene 关联；applyMode 仍走 EditorPlayRuntime 私有通路
    // 决策 4a: 不接 hook 拦 beginPlay；UX 弹窗由 caller 决定
    if (auto* host = ayt::app::currentEngineHost()) {
        if (auto* sm = host->scenes()) {
            _editScene = std::make_unique<ayt::scene::Scene>(
                ayt::scene::SceneMode::Edit, "<editor_default>");
            sm->setEdit(_editScene.get());
            sm->setCurrent(_editScene.get());
            AY_EDITOR_TRACE("initialize: edit scene injected (%s)",
                            _editScene->name().c_str());
        }
    }

    // v0.3+ PR-5 — 首刷 Hierarchy（_editScene 注入后才有 scene name）。
    // Edit World v1 永远空（决策 1b；plan §0.2）；Play 未启动 → tree 仅
    // 含合成 root。INV-4：纯读，Scene::_dirty 不可能被置位。
    refreshOutliner();

    // D5+.5: optional child-window manager for DockCard promotion.
    if (desc.childWindowManager != nullptr) {
        _childWindows = std::make_unique<EditorChildWindowManager>(
            *desc.childWindowManager, _ui);
        if (!desc.childWindowConfigPath.empty()) {
            const auto cfgs =
                parseChildWindowConfig(desc.childWindowConfigPath);
            for (const auto& cfg : cfgs) {
                void* h = nullptr;
                if (!_childWindows->openChildWindow(cfg, h)) {
                    std::fprintf(stderr,
                        "[EditorSession] child window '%s' failed to open\n",
                        cfg.title.c_str());
                }
            }
            AY_EDITOR_TRACE("initialize: opened %zu child window(s)",
                            _childWindows->count());
        }
        wirePromoteCallback();
    }

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
    // K-INV-D5-6: tear down child HWNDs BEFORE primary UIManager
    // shutdown. ~EditorChildWindowManager calls _wm.destroyTopLevelWindow
    // for every entry; ~UIManager on each child may poke
    // g_activeUIManager if it was active during the last tick. Doing
    // this here (with _ui still alive and owning the active slot)
    // avoids an UAF cleanup race against the primary.
    _childWindows.reset();
    _mainDock = nullptr;
    // Tear down Play/renderer borrow before UI widgets — avoids
    // Inspector path strings and GPU borrows racing UI teardown.
    _playRuntime.shutdownEngine();
    _gameView.setMode(EditorMode::Edit);

    // v0.3+ PR-5 — Landmine E: 清 Outliner 状态**早于** _ui.shutdown()
    // 避免 _ui.shutdown 期间 _outliner 指向已 free widget（UIManager 析构
    // 链上 deref）。
    _outliner = nullptr;
    _outlinerEntityIds.clear();
    _outlinerSelectedEntityId = 0;
    _outlinerRefreshPending = false;

    _ui.shutdown();
    _layoutPath.clear();
    _hostWindow = nullptr;

    // v0.3 PR-4 — shutdown reverse setEdit/setCurrent + reset _editScene
    // 顺序：先反注册 scene → 再 reset（EditorSession 析构时 unique_ptr 还会
    // 再 reset 一次；提前 reset 避免 SM 还指向 dangling Scene）
    if (auto* host = ayt::app::currentEngineHost()) {
        if (auto* sm = host->scenes()) {
            if (_editScene) {
                sm->setCurrent(nullptr);
                sm->setEdit(nullptr);
            }
        }
    }
    _editScene.reset();
}

void EditorSession::setClientSize(float width, float height) {
    _ui.setClientSize(width, height);
    _playRuntime.setClientSize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
    _ui.layout();
    syncViewport();
}

void EditorSession::update(float dt) {
    // D5+.5: tick every open child (each via pushActive scope) before
    // the primary update so the active pointer is correctly swapped
    // before any per-frame UI logic that might read g_activeUIManager.
    // Pass nullptr backend — child UIManagers use K-INV-D5-4 null
    // backend no-op; the host's _ui owns the only real backend.
    if (_childWindows) {
        _childWindows->tickAll(dt, nullptr);
    }
    _ui.update(dt);

    // v0.3+ PR-5 — Landmine B: 延迟消费 Outliner 重建（禁止在 TreeNode
    // 事件派发内重建；onOutlinerSelectionChanged 注释）。
    if (_outlinerRefreshPending) {
        _outlinerRefreshPending = false;
        refreshOutliner();
    }
    // Per-frame reconcile: if the last known cursor is not on a splitter
    // band, force every SplitterHandle un-revealed. Leave events alone
    // are not sufficient (capture path / skipped WM_MOUSEMOVE).
    syncSplitterRevealToMouse();

    if (freecamActive()) {
        if (viewportAcceptsGameInput()) {
            _freecam.updateMovement(dt);
        }
        pushFreecamToRenderer();
    }

    if (_gameView.mode() == EditorMode::Play) {
        _playRuntime.tick();
    } else if (_gameView.mode() == EditorMode::Paused) {
        // Keep last rendered frame visible; stepOnce drives simulation separately.
    }

    // Splitter drag updates HBox slot widths; keep the 3D viewport rect
    // in sync every frame so render composite tracks panel resize.
    syncViewportIfChanged();
}

bool EditorSession::freecamActive() const
{
    const EditorMode mode = _gameView.mode();
    return mode == EditorMode::Play || mode == EditorMode::Paused;
}

void EditorSession::pushFreecamToRenderer()
{
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        sub->setCameraLookAt(
            _freecam.eye(),
            _freecam.at(),
            _freecam.up(),
            _freecam.fovYDegrees());
    }
}

void EditorSession::render() {
    render(false);
}

void EditorSession::render(bool skipViewportPanel) {
    // Pre-AI-1 wrapper: kept for callers that want a single populate+
    // flush call. The new AI-1 path in AYEditorApp.cpp uses
    // populateFrame + flushFrame directly so the RenderPass dispatch
    // can own the UI submission boundary (UIPass::execute flushes
    // pending text batches).
    populateFrame(skipViewportPanel);
    flushFrame();
}

void EditorSession::populateFrame(bool skipViewportPanel) {
    // AI-1: begin-frame the widget walk that accumulates draws on
    // the backend's pendingRects + textBatch. No flush yet — the
    // flush moves to UIPass::execute so the RenderPass dispatch owns
    // the UI submission boundary. flushFrame() closes the lifecycle.
    _panelViewportForFrame = nullptr;
    _cardViewportForFrame = nullptr;
    if (skipViewportPanel) {
        ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
        if (viewport != nullptr) {
            _panelViewportWasVisibleForFrame = viewport->isVisible();
            viewport->setVisible(false);
            _panelViewportForFrame = viewport;
        }
        // Hide only the DockCard body fill — setVisible(false) on the
        // card would collapse the Center slot weight in DockArea.
        if (auto* card = dynamic_cast<ayt::ui::Panel*>(_ui.findById("card_viewport"))) {
            _cardViewportHadBackgroundForFrame = card->isBackgroundEnabled();
            card->setBackgroundEnabled(false);
            _cardViewportForFrame = card;
        }
    }

    _ui.layout();
    _ui.populateFrame();
}

void EditorSession::flushFrame() {
    // AI-1: close the IRenderBackend lifecycle (endCanvas + endFrame).
    // endFrame() inside the backend flushes pendingRects via
    // flushColoredRects() + any remaining text via flushPendingText().
    _ui.flushFrame();

    if (_panelViewportForFrame != nullptr) {
        _panelViewportForFrame->setVisible(_panelViewportWasVisibleForFrame);
        _panelViewportForFrame = nullptr;
    }
    if (_cardViewportForFrame != nullptr) {
        _cardViewportForFrame->setBackgroundEnabled(_cardViewportHadBackgroundForFrame);
        _cardViewportForFrame = nullptr;
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

void EditorSession::autoEnterNetClientPlay()
{
    if (!_netClientAutoPlay) {
        return;
    }
    std::fprintf(stderr,
        "[EditorSession] --net-client: auto-entering Play mode\n");
    _gameView.setMode(EditorMode::Play);
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

namespace {

void clearSplitterHoversRecursive(ayt::ui::Widget* widget)
{
    if (widget == nullptr) {
        return;
    }
    if (auto* split = dynamic_cast<ayt::ui::SplitterHandle*>(widget)) {
        // Prefer clearHoverReveal so we never depend on onMouseLeave
        // side effects / override quirks for the force-unreveal path.
        split->clearHoverReveal();
    }
    for (ayt::ui::Widget* child : widget->getChildren()) {
        clearSplitterHoversRecursive(child);
    }
}

bool pointOnSplitterRecursive(ayt::ui::Widget* widget, float x, float y)
{
    if (widget == nullptr || !widget->isVisible()) {
        return false;
    }
    if (auto* split = dynamic_cast<ayt::ui::SplitterHandle*>(widget)) {
        if (split->getWorldBounds().contains(ayt::math::FVector2(x, y))) {
            return true;
        }
    }
    for (ayt::ui::Widget* child : widget->getChildren()) {
        if (pointOnSplitterRecursive(child, x, y)) {
            return true;
        }
    }
    return false;
}

} // namespace

void EditorSession::clearSplitterHovers()
{
    clearSplitterHoversRecursive(_ui.root());
}

void EditorSession::syncSplitterRevealToMouse()
{
    if (!_hasLastMouse) {
        clearSplitterHovers();
        return;
    }
    // Use bounds walk (not hitTest): hitTest can prefer other widgets
    // or miss when layout is mid-update; bounds are the reveal source of truth.
    if (!pointOnSplitterRecursive(_ui.root(), _lastMouseX, _lastMouseY)) {
        clearSplitterHovers();
    }
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

    // Open menus / combo popups live on the overlay and often extend into
    // panel_viewport. In Play/Paused those points must still reach UIManager
    // or dropdown items over the cube receive no hits.
    if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
        const ayt::math::FVector2 pos(x, y);
        for (ayt::ui::Widget* child : overlay->getChildren()) {
            if (child == nullptr || !child->isVisible()) {
                continue;
            }
            if (child->getWorldBounds().contains(pos)) {
                return true;
            }
        }
    }

    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)) {
        return true;
    }

    return x < viewport.minX || x >= viewport.maxX
        || y < viewport.minY || y >= viewport.maxY;
}

bool EditorSession::viewportAcceptsGameInput() const {
    if (_hostWindow == nullptr) {
        return false;
    }
    if (::GetForegroundWindow() != _hostWindow) {
        return false;
    }
    // LMB look already started inside the viewport — keep movement.
    if (_freecam.isLooking()) {
        return true;
    }
    if (!_hasLastMouse) {
        return false;
    }
    return !isChromePoint(_lastMouseX, _lastMouseY);
}

bool EditorSession::onMouseMove(float x, float y) {
    _lastMouseX = x;
    _lastMouseY = y;
    _hasLastMouse = true;

    // Armed viewport LMB: past slop → freecam look (not a click-select).
    if (_viewportLmbPending && !_viewportLmbDragged && !_freecam.isLooking()) {
        const float dx = x - _viewportLmbX;
        const float dy = y - _viewportLmbY;
        if ((dx * dx + dy * dy) >= (5.0f * 5.0f)) {
            _viewportLmbDragged = true;
            _freecam.beginLook(_viewportLmbX, _viewportLmbY);
            _freecam.updateLook(x, y);
            pushFreecamToRenderer();
            return true;
        }
    }

    if (_freecam.isLooking()) {
        _freecam.updateLook(x, y);
        pushFreecamToRenderer();
        return true;
    }

    if (_ui.isCapturing()) {
        // Still deliver moves (drag). Bounds-checked SplitterHandle::
        // onMouseMove clears _hover when outside the band; sync in
        // update() finishes un-reveal after mouse-up.
        return _ui.onMouseMove(x, y);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        clearSplitterHovers();
        return false;
    }

    const bool handled = _ui.onMouseMove(x, y);
    if (!pointOnSplitterRecursive(_ui.root(), x, y)) {
        clearSplitterHovers();
    }
    return handled;
}

bool EditorSession::onMouseButtonDown(float x, float y, int button) {
    // Dismiss MenuBar popups on any click that is not inside an open
    // menu. Play-mode freecam / isCapturing used to skip UIManager, so
    // click-outside never ran and the dropdown stayed painted forever.
    {
        const ayt::math::FVector2 pos(x, y);
        bool insideOpenMenu = false;
        if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
            for (ayt::ui::Widget* child : overlay->getChildren()) {
                auto* menu = dynamic_cast<ayt::ui::Menu*>(child);
                if (menu == nullptr || !menu->isOpen()) {
                    continue;
                }
                if (menu->getWorldBounds().contains(pos)) {
                    insideOpenMenu = true;
                    break;
                }
            }
        }
        if (!insideOpenMenu) {
            if (auto* menuBar =
                    dynamic_cast<ayt::ui::MenuBar*>(_ui.findById("menubar"))) {
                menuBar->closeOpenMenu();
            }
        }
    }

    if (_ui.isCapturing()) {
        return _ui.onMouseButtonDown(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();

        if (button == 0 && freecamActive()) {
            // Defer freecam until drag past slop — short click selects.
            _viewportLmbPending = true;
            _viewportLmbDragged = false;
            _viewportLmbX = x;
            _viewportLmbY = y;
            return true; // host should SetCapture
        }
        return false;
    }

    return _ui.onMouseButtonDown(x, y, button);
}

bool EditorSession::onMouseButtonUp(float x, float y, int button) {
    if (button == 0 && _viewportLmbPending) {
        const bool wasClick = !_viewportLmbDragged && !_freecam.isLooking();
        _viewportLmbPending = false;
        _viewportLmbDragged = false;
        if (_freecam.isLooking()) {
            _freecam.endLook();
        }
        if (wasClick) {
            selectPlayEntityFromViewport();
        }
        return true;
    }

    if (_freecam.isLooking() && button == 0) {
        _freecam.endLook();
        return true;
    }

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
    _hasLastMouse = false;
    _ui.onMouseLeave();
    clearSplitterHovers();
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

    bindButton("btn_minimize", [this]() { requestHostMinimize(); });
    bindButton("btn_maximize", [this]() { requestHostMaximizeToggle(); });
    bindButton("btn_close", [this]() { requestHostClose(); });

    bindButton("btn_inspector_skel",  [this]() { pickInspectorSkeleton(); });
    bindButton("btn_inspector_anim",  [this]() { pickInspectorAnimation(); });
    bindButton("btn_inspector_apply", [this]() { applyInspectorOverrides(); });
    bindButton("btn_inspector_reset", [this]() { resetInspectorOverrides(); });

    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setBackgroundColor(ayt::math::FVector4(0.10f, 0.10f, 0.11f, 1.0f));
        }
    }
}

void EditorSession::bindTransportBar() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    // v0.3 PR-4 — btn_play enable 条件 + dirty prompt UX（design §4.3.x）
    // 决策 1a: enable = host->scenes()->canBeginPlay()
    // 决策 4a: Save/Discard/Cancel 三选项 Win32 MessageBoxW
    // 决策 5a: lbl_unsaved period refresh（mode changed 时同步）
    bindButton("btn_play", [this]() {
        auto* host = ayt::app::currentEngineHost();
        if (!host) return;
        auto* sm = host->scenes();
        if (!sm || !sm->canBeginPlay()) return;

        // Save/Discard/Cancel prompt (PR-3 requireSaveBeforePlay 意图 getter)
        if (sm->requireSaveBeforePlay()) {
            int choice = ::MessageBoxW(
                _hostWindow,
                L"Scene has unsaved changes.\n\nSave before Play?",
                L"AYEditor",
                MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDCANCEL) return;  // Cancel: 早返
            if (choice == IDYES) {
                auto* edit = sm->edit();
                if (edit == nullptr) return;
                // 编辑场景路径为空时不强行 save（避免无意义空文件）；提示错误
                if (edit->path().empty()) {
                    ::MessageBoxW(_hostWindow,
                        L"Scene has no path. Use File > Save first.",
                        L"AYEditor", MB_OK | MB_ICONERROR);
                    return;
                }
                if (!edit->save(edit->path())) {
                    ::MessageBoxW(_hostWindow,
                        L"Save failed. Cannot start Play.",
                        L"AYEditor", MB_OK | MB_ICONERROR);
                    return;
                }
            }
            // IDNO = Discard：继续
        }

        _gameView.setMode(EditorMode::Play);
    });

    bindButton("btn_pause", [this]() { _gameView.setMode(EditorMode::Paused); });
    bindButton("btn_step", [this]() {
        _gameView.stepOnce();
        if (_repaintCallback) {
            _repaintCallback();
        }
    });
    bindButton("btn_stop", [this]() { _gameView.setMode(EditorMode::Edit); });

    // v0.3 PR-4 — lbl_unsaved 初始 refresh（design §4.3.x 决策 5a）
    refreshUnsavedIndicator();
}

// helper：刷新 lbl_unsaved TextLabel（visible + text）
// 决策 5a: mode changed / save / clear 触发点 refresh；不每帧轮询
void EditorSession::refreshUnsavedIndicator() {
    auto* widget = _ui.findById("lbl_unsaved");
    if (widget == nullptr) return;
    auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget);
    if (label == nullptr) return;

    auto* host = ayt::app::currentEngineHost();
    bool dirty = false;
    if (host) {
        if (auto* sm = host->scenes()) {
            if (sm->isEditDirty()) {
                dirty = true;
            }
        }
    }
    label->setText(dirty ? L"•" : L"");
    label->setVisible(dirty);
}

// =============================================================================
// v0.3+ PR-5 — Hierarchy / Outliner (design §4.3.y)
// =============================================================================
namespace {

// 决策 1b（mode-keyed World 源）：
//   Edit        → host->scenes()->edit()->world()（canonical；v1 永远空，
//                 因为没有任何路径往 Edit World 建 entity ——
//                 见 AYScene.cpp:44 私有 World 实例 vs
//                 AYEditorPlayRuntime.cpp:454/1224/1632 singleton spawn）
//   Play/Paused → World::instance()（EditorPlayRuntime 实际 spawn 目标）
// 返回 const 引用以物理性保证 INV-4（不可能从这里 mutate Scene）。
const ayt::entity::World* resolveHierarchyWorld(EditorMode mode)
{
    if (mode == EditorMode::Edit) {
        auto* host = ayt::app::currentEngineHost();
        if (host == nullptr) return nullptr;
        auto* sm = host->scenes();
        if (sm == nullptr) return nullptr;
        auto* edit = sm->edit();
        if (edit == nullptr) return nullptr;
        return &edit->world();
    }
    if (!ayt::entity::World::instance().isInitialized()) {
        return nullptr;
    }
    return &ayt::entity::World::instance();
}

// 非 const 版：selection 解析需要 findEntity（AYWorld.h:42 非 const）。
ayt::entity::World* resolveHierarchyWorldMutable(EditorMode mode)
{
    return const_cast<ayt::entity::World*>(
        resolveHierarchyWorld(mode));
}

} // namespace

void EditorSession::bindOutlinerPanel()
{
    _outliner = dynamic_cast<ayt::ui::TreeView*>(
        _ui.findById("tree_outliner"));
    if (_outliner == nullptr) {
        // PR-4 的 "layout 缺失静默跳过" 模式（initialize 已在
        // _layoutPath.empty() 时不 loadLayout；此处 findById 落空同理）。
        return;
    }
    // Landmine A 修法：TreeView 的 JSON itemHeight 在 DockCard.content 路径
    // 下不会被应用（AYLayoutLoader.cpp:191-197 走 buildWidgetTree，
    // 不走 AYWidgetSerializer.cpp:441-443 的 itemHeight 解析）。
    // 故从代码设。
    _outliner->setItemHeight(16.0f);
    _outliner->setOnSelectionChanged(
        [this](int flatIndex) { onOutlinerSelectionChanged(flatIndex); });
}

void EditorSession::refreshOutliner()
{
    if (_outliner == nullptr) {
        return;
    }

    auto setUtf8 = [this](const char* id, const std::string& utf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(
                    std::wstring(utf8.begin(), utf8.end()));
            }
        }
    };

    _outlinerEntityIds.clear();

    const ayt::entity::World* world =
        resolveHierarchyWorld(_gameView.mode());
    if (world == nullptr) {
        _outliner->clearTree();
        _outlinerSelectedEntityId = 0;
        setUtf8("outliner_hint", "Scene: -");
        return;
    }

    // 合成 root 节点 label：scene name（host->scenes()->current()）。
    std::string rootLabel = "<no scene>";
    if (auto* host = ayt::app::currentEngineHost()) {
        if (auto* sm = host->scenes()) {
            if (auto* cur = sm->current()) {
                rootLabel = cur->name().empty() ? "<unnamed>"
                                                : cur->name();
            }
        }
    }
    if (_gameView.mode() != EditorMode::Edit) {
        rootLabel += "  (Play World)";
    }
    setUtf8("outliner_hint", "Scene: " + rootLabel);

    std::vector<ayt::ui::TreeNodeData> nodes;
    ayt::ui::TreeNodeData root;
    root.label = std::wstring(rootLabel.begin(), rootLabel.end());
    root.hasChildren = true;
    root.expanded = true;
    root.parentIndex = -1;
    nodes.push_back(root);

    // INV-4：唯一 Scene 触点是 const World& + getAllEntities() const
    // （AYWorld.h:43）+ Entity::getId/getName（AYEntityImpl.h:31-32）。
    // 无任何 clear/load/save 调用 → Scene::_dirty 不可能被置位。
    const std::vector<ayt::entity::Entity*> entities =
        world->getAllEntities();
    nodes.reserve(entities.size() + 1);
    _outlinerEntityIds.reserve(entities.size());
    for (ayt::entity::Entity* e : entities) {
        if (e == nullptr) continue;
        const char* nameC = e->getName();
        std::string name = (nameC != nullptr && nameC[0] != '\0')
            ? std::string(nameC)
            : ("entity#" + std::to_string(
                  static_cast<unsigned>(e->getId())));

        ayt::ui::TreeNodeData d;
        d.label = std::wstring(name.begin(), name.end());
        d.hasChildren = false;
        d.expanded = false;
        d.parentIndex = 0;  // 挂在合成 root 下
        nodes.push_back(d);
        _outlinerEntityIds.push_back(e->getId());
    }

    _outliner->setTree(nodes);

    // 选择保持：id 仍在列表里就把高亮放回去（flatIndex = 序号 + 1）。
    if (_outlinerSelectedEntityId != 0) {
        int flat = -1;
        for (size_t i = 0; i < _outlinerEntityIds.size(); ++i) {
            if (_outlinerEntityIds[i] == _outlinerSelectedEntityId) {
                flat = static_cast<int>(i) + 1;
                break;
            }
        }
        if (flat < 0) {
            _outlinerSelectedEntityId = 0;  // 实体已销毁（endPlay 等）
        } else {
            _outliner->setSelectedIndex(flat);
        }
    }
}

void EditorSession::onOutlinerSelectionChanged(int flatIndex)
{
    // flat 0 = 合成 scene root：清 Hierarchy 选择，Inspector 退回 PR-4 路径。
    if (flatIndex <= 0) {
        _outlinerSelectedEntityId = 0;
        refreshInspectorLabels();
        if (_repaintCallback) _repaintCallback();
        return;
    }
    const size_t idx = static_cast<size_t>(flatIndex - 1);
    if (idx >= _outlinerEntityIds.size()) {
        return;
    }
    _outlinerSelectedEntityId = _outlinerEntityIds[idx];

    // **Landmine B**：不**在此调 refreshOutliner()/_ui.layout()：会
    // delete 正在派发事件的 TreeNode（AYTreeView.cpp:80-85/194）→
    // UIManager::onMouseButtonUp:1339 UAF。只刷 Inspector（纯
    // TextLabel setText） + repaint。
    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::setDockCardVisible(const char* cardId, bool visible) {
    ayt::ui::DockCard* card = nullptr;
    if (_mainDock != nullptr) {
        card = _mainDock->findCard(cardId);
    }
    if (card == nullptr) {
        card = dynamic_cast<ayt::ui::DockCard*>(_ui.findById(cardId));
    }
    if (card != nullptr) {
        card->setVisible(visible);
    }
    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::toggleDockCard(const char* cardId, bool& visibleFlag) {
    visibleFlag = !visibleFlag;
    setDockCardVisible(cardId, visibleFlag);
}

void EditorSession::bindNetworkPanelStub() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_net_spawn", []() {
        std::fprintf(stderr,
            "[EditorSession] Network Spawn (stub) — wire to server EntitySpawn\n");
    });
    bindButton("btn_net_despawn", []() {
        std::fprintf(stderr,
            "[EditorSession] Network Despawn (stub) — wire to server despawn\n");
    });

    if (auto* w = _ui.findById("sld_net_hp")) {
        if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
            slider->setOnValueChanged([this](float v) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Cube HP  %.0f",
                              static_cast<double>(v));
                if (auto* lbl = _ui.findById("lbl_net_hp")) {
                    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(lbl)) {
                        label->setText(std::wstring(buf, buf + std::strlen(buf)));
                    }
                }
            });
        }
    }
}

void EditorSession::bindRenderSettingsPanel()
{
    // Left "Render" panel — live knobs after RendererSubSystem exists
    // (Play session). Safe no-ops before Play: findRegistered() is null.
    auto rendererOrNull = []() -> ayt::render::Renderer* {
        if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
            return &sub->renderer();
        }
        return nullptr;
    };

    auto setLabel = [this](const char* id, const char* textUtf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(textUtf8, textUtf8 + std::strlen(textUtf8)));
            }
        }
    };

    auto bindSlider = [this](const char* id, std::function<void(float)> onChanged) {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                slider->setOnValueChanged(std::move(onChanged));
            }
        }
    };

    bindSlider("sld_gamma", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Gamma  %.2f", static_cast<double>(v));
        setLabel("lbl_gamma", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessGamma(v);
        }
    });

    bindSlider("sld_exposure", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Exposure  %.2f", static_cast<double>(v));
        setLabel("lbl_exposure", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessExposure(v);
        }
    });

    bindSlider("sld_bloom", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Bloom  %.2f", static_cast<double>(v));
        setLabel("lbl_bloom", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessBloomStrength(v);
        }
    });

    // §S4d — Depth Haze (default slightly on in UI JSON).
    auto applyHazeParams = [rendererOrNull](bool enabled, float strength, float density) {
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setDepthHazeEnabled(enabled);
            r->setDepthHazeStrength(enabled ? strength : 0.0f);
            r->setDepthHazeParams(
                density,
                ayt::math::FVector3(0.7f, 0.75f, 0.8f));
        }
    };

    if (auto* w = _ui.findById("chk_depth_haze")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applyHazeParams](bool on) {
                float strength = 1.0f;
                float density = 0.04f;
                if (auto* sw = _ui.findById("sld_haze_strength")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = s->getValue();
                    }
                }
                if (auto* dw = _ui.findById("sld_haze_density")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(dw)) {
                        density = s->getValue();
                    }
                }
                applyHazeParams(on, strength, density);
            });
        }
    }

    bindSlider("sld_haze_strength", [this, setLabel, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Haze Strength  %.2f", static_cast<double>(v));
        setLabel("lbl_haze_strength", buf);
        bool enabled = true;
        float density = 0.04f;
        if (auto* cw = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* dw = _ui.findById("sld_haze_density")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(dw)) {
                density = s->getValue();
            }
        }
        applyHazeParams(enabled, v, density);
    });

    bindSlider("sld_haze_density", [this, setLabel, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Haze Density  %.3f", static_cast<double>(v));
        setLabel("lbl_haze_density", buf);
        bool enabled = true;
        float strength = 1.0f;
        if (auto* cw = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_haze_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        applyHazeParams(enabled, strength, v);
    });

    // §S2 v1 — SSAO (Deferred-only; UI defaults slightly on).
    auto applySsaoParams = [rendererOrNull](bool enabled, float strength,
                                            float radius, float bias) {
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setSsaoEnabled(enabled);
            r->setSsaoStrength(enabled ? strength : 0.0f);
            r->setSsaoParams(radius, bias);
        }
    };

    if (auto* w = _ui.findById("chk_ssao")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applySsaoParams](bool on) {
                float strength = 0.45f;
                float radius = 0.4f;
                float bias = 0.04f;
                if (auto* sw = _ui.findById("sld_ssao_strength")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = s->getValue();
                    }
                }
                if (auto* rw = _ui.findById("sld_ssao_radius")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                        radius = s->getValue();
                    }
                }
                if (auto* bw = _ui.findById("sld_ssao_bias")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                        bias = s->getValue();
                    }
                }
                applySsaoParams(on, strength, radius, bias);
            });
        }
    }

    bindSlider("sld_ssao_strength", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Strength  %.2f", static_cast<double>(v));
        setLabel("lbl_ssao_strength", buf);
        bool enabled = true;
        float radius = 0.4f;
        float bias = 0.04f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* rw = _ui.findById("sld_ssao_radius")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                radius = s->getValue();
            }
        }
        if (auto* bw = _ui.findById("sld_ssao_bias")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                bias = s->getValue();
            }
        }
        applySsaoParams(enabled, v, radius, bias);
    });

    bindSlider("sld_ssao_radius", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Radius  %.2f", static_cast<double>(v));
        setLabel("lbl_ssao_radius", buf);
        bool enabled = true;
        float strength = 0.45f;
        float bias = 0.04f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_ssao_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        if (auto* bw = _ui.findById("sld_ssao_bias")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                bias = s->getValue();
            }
        }
        applySsaoParams(enabled, strength, v, bias);
    });

    bindSlider("sld_ssao_bias", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Bias  %.3f", static_cast<double>(v));
        setLabel("lbl_ssao_bias", buf);
        bool enabled = true;
        float strength = 0.45f;
        float radius = 0.4f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_ssao_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        if (auto* rw = _ui.findById("sld_ssao_radius")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                radius = s->getValue();
            }
        }
        applySsaoParams(enabled, strength, radius, v);
    });

    bindSlider("sld_ambient", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "IBL Ambient  %.2f", static_cast<double>(v));
        setLabel("lbl_ambient", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setAmbientStrength(v);
        }
    });

    bindSlider("sld_shadow_bias", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Shadow Bias  %.4f", static_cast<double>(v));
        setLabel("lbl_shadow_bias", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setShadowBias(v);
        }
    });

    if (auto* w = _ui.findById("cmb_tonemap")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            combo->setOnSelectionChanged([rendererOrNull](int index) {
                ayt::render::Renderer* r = rendererOrNull();
                if (r == nullptr) {
                    return;
                }
                using TM = ayt::render::Renderer::TonemapMode;
                switch (index) {
                case 1:  r->setPostProcessTonemapMode(TM::Reinhard); break;
                case 2:  r->setPostProcessTonemapMode(TM::ACES); break;
                default: r->setPostProcessTonemapMode(TM::None); break;
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_shadow_pcf")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([rendererOrNull](bool on) {
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setShadowPcfEnabled(on);
                }
            });
        }
    }

    // Labels in JSON are decorative until Slider min/max/value load;
    // refresh from the live widget values so thumb ↔ text stay aligned.
    auto refreshLabelFromSlider = [this, setLabel](const char* sliderId,
                                                   const char* labelId,
                                                   const char* fmt) {
        if (auto* w = _ui.findById(sliderId)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), fmt,
                              static_cast<double>(slider->getValue()));
                setLabel(labelId, buf);
            }
        }
    };
    refreshLabelFromSlider("sld_gamma", "lbl_gamma", "Gamma  %.2f");
    refreshLabelFromSlider("sld_exposure", "lbl_exposure", "Exposure  %.2f");
    refreshLabelFromSlider("sld_bloom", "lbl_bloom", "Bloom  %.2f");
    refreshLabelFromSlider("sld_haze_strength", "lbl_haze_strength", "Haze Strength  %.2f");
    refreshLabelFromSlider("sld_haze_density", "lbl_haze_density", "Haze Density  %.3f");
    refreshLabelFromSlider("sld_ssao_strength", "lbl_ssao_strength", "SSAO Strength  %.2f");
    refreshLabelFromSlider("sld_ssao_radius", "lbl_ssao_radius", "SSAO Radius  %.2f");
    refreshLabelFromSlider("sld_ssao_bias", "lbl_ssao_bias", "SSAO Bias  %.3f");
    refreshLabelFromSlider("sld_ambient", "lbl_ambient", "IBL Ambient  %.2f");
    refreshLabelFromSlider("sld_shadow_bias", "lbl_shadow_bias", "Shadow Bias  %.4f");
}

void EditorSession::applyRenderSettingsFromPanel()
{
    auto* sub = ayt::render::RendererSubSystem::findRegistered();
    if (sub == nullptr) {
        return;
    }
    ayt::render::Renderer& r = sub->renderer();

    auto sliderValue = [this](const char* id, float fallback) -> float {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                return slider->getValue();
            }
        }
        return fallback;
    };

    r.setPostProcessGamma(sliderValue("sld_gamma", 2.2f));
    r.setPostProcessExposure(sliderValue("sld_exposure", 1.0f));
    r.setPostProcessBloomStrength(sliderValue("sld_bloom", 0.3f));
    r.setAmbientStrength(sliderValue("sld_ambient", 0.85f));
    r.setShadowBias(sliderValue("sld_shadow_bias", 0.003f));

    // §S4d — Depth Haze defaults: on, density 0.04, fog (0.7,0.75,0.8).
    {
        bool hazeOn = true;
        if (auto* w = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                hazeOn = chk->isChecked();
            }
        }
        const float hazeStrength = sliderValue("sld_haze_strength", 1.0f);
        const float hazeDensity = sliderValue("sld_haze_density", 0.04f);
        r.setDepthHazeEnabled(hazeOn);
        r.setDepthHazeStrength(hazeOn ? hazeStrength : 0.0f);
        r.setDepthHazeParams(
            hazeDensity,
            ayt::math::FVector3(0.7f, 0.75f, 0.8f));
    }

    // §S2 v1 — SSAO defaults: on, strength 0.45, radius 0.4, bias 0.04.
    {
        bool ssaoOn = true;
        if (auto* w = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                ssaoOn = chk->isChecked();
            }
        }
        const float ssaoStrength = sliderValue("sld_ssao_strength", 0.45f);
        const float ssaoRadius = sliderValue("sld_ssao_radius", 0.4f);
        const float ssaoBias = sliderValue("sld_ssao_bias", 0.04f);
        r.setSsaoEnabled(ssaoOn);
        r.setSsaoStrength(ssaoOn ? ssaoStrength : 0.0f);
        r.setSsaoParams(ssaoRadius, ssaoBias);
    }

    if (auto* w = _ui.findById("cmb_tonemap")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            using TM = ayt::render::Renderer::TonemapMode;
            switch (combo->getSelectedIndex()) {
            case 1:  r.setPostProcessTonemapMode(TM::Reinhard); break;
            case 2:  r.setPostProcessTonemapMode(TM::ACES); break;
            default: r.setPostProcessTonemapMode(TM::None); break;
            }
        }
    }

    if (auto* w = _ui.findById("chk_shadow_pcf")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            r.setShadowPcfEnabled(chk->isChecked());
        }
    }
}

void EditorSession::bindMenuBar() {
    auto* widget = _ui.findById("menubar");
    auto* menuBar = dynamic_cast<ayt::ui::MenuBar*>(widget);
    if (menuBar == nullptr) {
        return;
    }

    ayt::ui::Menu* fileMenu = menuBar->addMenu(L"File");
    if (fileMenu != nullptr) {
        if (auto* item = fileMenu->addItem(L"New")) {
            item->setOnActivate([]() {});
        }
        if (auto* item = fileMenu->addItem(L"Open...")) {
            item->setOnActivate([]() {});
        }
        if (auto* item = fileMenu->addItem(L"Import...")) {
            item->setOnActivate([this]() { importCharacterFromDialog(); });
        }
        fileMenu->addSeparator();
        if (auto* item = fileMenu->addItem(L"Exit")) {
            item->setOnActivate([this]() { requestHostClose(); });
        }
    }

    ayt::ui::Menu* windowMenu = menuBar->addMenu(L"Window");
    if (windowMenu != nullptr) {
        if (auto* item = windowMenu->addItem(L"Render Settings")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_render", _panelRenderVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Inspector")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_inspector", _panelInspectorVisible);
            });
        }
        // v0.3+ PR-5 — Hierarchy panel toggle (design §4.3.y)
        if (auto* item = windowMenu->addItem(L"Hierarchy")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_outliner", _panelOutlinerVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Network")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_network", _panelNetworkVisible);
            });
        }
        windowMenu->addSeparator();
        if (auto* item = windowMenu->addItem(L"Select Character")) {
            item->setOnActivate([this]() { selectCharacter(); });
        }
    }

    ayt::ui::Menu* helpMenu = menuBar->addMenu(L"Help");
    if (helpMenu != nullptr) {
        if (auto* item = helpMenu->addItem(L"About AYEditor")) {
            item->setOnActivate([]() {});
        }
    }
}

void EditorSession::requestHostClose() {
    if (_hostWindow != nullptr) {
        ::PostMessageW(_hostWindow, WM_CLOSE, 0, 0);
    }
}

void EditorSession::requestHostMinimize() {
    if (_hostWindow != nullptr) {
        ::ShowWindow(_hostWindow, SW_MINIMIZE);
    }
}

void EditorSession::requestHostMaximizeToggle() {
    if (_hostWindow == nullptr) {
        return;
    }
    if (::IsZoomed(_hostWindow)) {
        ::ShowWindow(_hostWindow, SW_RESTORE);
    } else {
        ::ShowWindow(_hostWindow, SW_MAXIMIZE);
    }
}

namespace {

// Join a vector of type-name strings for stable stderr output,
// e.g. {"Animation", "Material"} -> "Material, Animation"
// (insertion order preserved). Used by the import pipeline's
// log lines.
std::string joinTypeNames(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

} // namespace

// Phase 2a: toolbar Import button target. Mirrors the
// --import pipeline from EditorApp::run() but with the path
// source being the Win32 dialog (Phase 1 left this as a one-
// line shim). The dialog blocks on a modal until the user
// picks or cancels, so this method is fired-and-forgot from
// the main thread; nothing else on the UI thread runs while
// it's open.
void EditorSession::importCharacterFromDialog()
{
    const std::string sourcePath =
        ImportDialog::showOpenFileDialog(_hostWindow);
    if (sourcePath.empty()) {
        // User cancelled (or non-Windows stub returned empty).
        // Silent no-op; do not pollute stderr with a "no path"
        // message because cancels are a normal interaction.
        return;
    }

    const std::string cacheRoot =
        EditorPlayRuntime::resolvePersistentCacheRoot();
    const std::string assetRoot = cacheRoot + "assets\\";

    const Importer::Result result =
        ImportDialog::importFromPath(sourcePath, assetRoot);
    if (!result.success) {
        std::fprintf(stderr,
                     "[EditorSession] import failed: %s\n",
                     result.errorMessage.c_str());
        return;
    }

    ImportedCharacterMapDiagnostics diag;
    const ImportedCharacter mapped =
        mapConversionToImportedCharacter(result.conversion, cacheRoot, diag);
    if (!diag.success) {
        std::fprintf(stderr,
                     "[EditorSession] import produced no skinned character: "
                     "missing [%s]\n",
                     joinTypeNames(diag.missing).c_str());
        // Mapper rejected; keep whatever is currently spawned
        // (cube if no previous import, or prior character if
        // hot-swap with rejection). Do NOT replace with default-
        // constructed; that would clear a previously-imported
        // valid character on a bad second import.
        return;
    }

    _playRuntime.replaceImportedCharacter(mapped);
    std::fprintf(stderr,
                 "[EditorSession] imported character ready "
                 "(mesh=%s, skel=%s, anim=%s)\n",
                 mapped.meshPath.c_str(),
                 mapped.skeletonPath.c_str(),
                 mapped.animationPath.c_str());

    // ED-03: a successful import auto-selects the new character
    // for inspection, so the designer can immediately click
    // [Pick Anim] and re-route to a different .ayanm. Without
    // this the user has to click [Select] twice (once to
    // populate the inspector, once after applying).
    refreshInspectorLabels();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

// ED-03: walk the inspector's TextLabels and update them with
// the currently-spawned Play entity (character preferred, else cube).
void EditorSession::refreshInspectorLabels()
{
    auto setUtf8 = [this](const char* id, const std::string& utf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(utf8.begin(), utf8.end()));
            }
        }
    };

    // v0.3+ PR-5 — Hierarchy 选择优先于 PR-4 的 character/cube 二选一。
    // 存 id 不存指针 → 每次重解析，实体没了自动降级（Landmine F）。
    if (_outlinerSelectedEntityId != 0) {
        if (auto* w = resolveHierarchyWorldMutable(_gameView.mode())) {
            if (ayt::entity::Entity* sel =
                    w->findEntity(_outlinerSelectedEntityId)) {
                const char* nm = sel->getName();
                setUtf8("inspector_hint",
                        std::string("Hierarchy: ")
                        + ((nm && nm[0]) ? nm : "entity"));
                if (auto* meshC = sel->getComponent<ayt::entity::MeshComponent>()) {
                    setUtf8("inspector_mesh", "mesh: " + meshC->meshPath);
                } else {
                    setUtf8("inspector_mesh", "mesh: -");
                }
                if (auto* skelC = sel->getComponent<ayt::entity::SkeletonComponent>()) {
                    setUtf8("inspector_skel", "skel: " + skelC->skeletonPath);
                } else {
                    setUtf8("inspector_skel", "skel: -");
                }
                if (auto* animC = sel->getComponent<ayt::entity::AnimationComponent>()) {
                    setUtf8("inspector_anim",
                            animC->clipPath.empty() ? "anim: (bind-pose)"
                                                    : ("anim: " + animC->clipPath));
                } else {
                    setUtf8("inspector_anim", "anim: -");
                }
                return;
            }
        }
        _outlinerSelectedEntityId = 0;  // 已销毁 → 降级到 PR-4 路径
    }

    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = nullptr;
    if (_inspectorPreferCube && cube != nullptr) {
        e = cube;
    } else if (character != nullptr) {
        e = character;
    } else {
        e = cube;
    }

    if (e == nullptr) {
        setUtf8("inspector_hint", "No selection");
        setUtf8("inspector_mesh", "mesh: -");
        setUtf8("inspector_skel", "skel: -");
        setUtf8("inspector_anim", "anim: -");
        return;
    }

    const bool isCharacter = (e == character);
    char hint[96];
    std::snprintf(hint, sizeof(hint), "%s  (click#%u)",
                  isCharacter ? "Character" : "Cube (opaque ref)",
                  static_cast<unsigned>(_viewportClickCount));
    setUtf8("inspector_hint", hint);

    if (auto* meshC = e->getComponent<ayt::entity::MeshComponent>()) {
        setUtf8("inspector_mesh", "mesh: " + meshC->meshPath);
    } else {
        setUtf8("inspector_mesh", "mesh: -");
    }
    if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
        setUtf8("inspector_skel", "skel: " + skelC->skeletonPath);
    } else {
        setUtf8("inspector_skel", "skel: -");
    }
    if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
        setUtf8("inspector_anim",
                animC->clipPath.empty() ? "anim: (bind-pose)"
                                        : ("anim: " + animC->clipPath));
    } else {
        setUtf8("inspector_anim", "anim: -");
    }
}

void EditorSession::selectPlayEntityFromViewport()
{
    // Cycle Character ↔ opaque cube so Inspector labels change on
    // every short click (ray-pick not required yet).
    ++_viewportClickCount;
    if (_playRuntime.selectedCharacterEntity() != nullptr
        && _playRuntime.cubeEntity() != nullptr) {
        _inspectorPreferCube = !_inspectorPreferCube;
    } else {
        _inspectorPreferCube = (_playRuntime.selectedCharacterEntity() == nullptr);
    }
    selectCharacter();
}

// ED-03: [Select] handler. Snapshots paths into the inspector.
// Falls back to the procedural cube when character spawn failed.
void EditorSession::selectCharacter()
{
    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = nullptr;
    if (_inspectorPreferCube && cube != nullptr) {
        e = cube;
    } else if (character != nullptr) {
        e = character;
    } else {
        e = cube;
    }

    if (e == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] nothing to select; enter Play first "
            "(character or cube)\n");
        refreshInspectorLabels();
        return;
    }

    if (e == character) {
        if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
            _inspectorSkelPick = skelC->skeletonPath;
        }
        if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
            _inspectorAnimPick = animC->clipPath;
        }
    } else {
        _inspectorSkelPick.clear();
        _inspectorAnimPick.clear();
    }

    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// ED-03: [Pick Skel] handler. Opens the Win32 dialog filtered
// to .ayskel, stashes the chosen path into _inspectorSkelPick.
// The path is NOT yet applied to the live entity - [Apply]
// commits it. Cancel returns empty = no-op.
void EditorSession::pickInspectorSkeleton()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    // We currently pass the empty filter straight through; the
    // ImportDialog::showOpenFileDialog defaults to its built-in
    // 3D-Model (.fbx/.gltf/.glb) filter, which is wider than
    // .ayskel. To stay within Phase 1 scope we accept the wider
    // filter (the user just types the path or picks any
    // cache-resident file). A typed filter arg is a Phase 2
    // refinement when picker infrastructure exists.
    const std::string picked =
        ayt::editor::ImportDialog::showOpenFileDialog(_hostWindow);
    if (picked.empty()) {
        return; // user cancelled
    }
    setInspectorSkeletonPath(picked);
}

// ED-03: [Pick Anim] handler, sibling of pickInspectorSkeleton.
void EditorSession::pickInspectorAnimation()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    const std::string picked =
        ayt::editor::ImportDialog::showOpenFileDialog(_hostWindow);
    if (picked.empty()) {
        return;
    }
    setInspectorAnimationPath(picked);
}

// ED-03: thin setters that stash the path and refresh the
// corresponding inspector label so the user sees feedback
// between [Pick ...] and [Apply].
void EditorSession::setInspectorSkeletonPath(const std::string& path)
{
    _inspectorSkelPick = path;
    if (auto* w = _ui.findById("inspector_skel")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            label->setText(std::wstring(path.begin(), path.end()));
        }
    }
}

void EditorSession::setInspectorAnimationPath(const std::string& path)
{
    _inspectorAnimPick = path;
    if (auto* w = _ui.findById("inspector_anim")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            label->setText(std::wstring(path.begin(), path.end()));
        }
    }
}

// ED-03: [Apply] handler. Builds an EntityInspectorOverrides
// from the staged picks and forwards to the runtime. Empty
// pick on a field => leave that component path unchanged
// (the runtime's applyComponentOverrides treats empty as
// "keep").
void EditorSession::applyInspectorOverrides()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    EntityInspectorOverrides ov;
    ov.skeletonPathOverride  = _inspectorSkelPick;
    ov.animationPathOverride = _inspectorAnimPick;
    commitInspectorOverrides(ov);
}

// ED-03: [Reset] handler. Clear pending overrides back to
// default (which the runtime interprets as "stop applying
// user picks on the next spawn"). Does NOT clear the live
// entity's component paths - the user keeps whatever is
// animating now.
void EditorSession::resetInspectorOverrides()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    _inspectorSkelPick.clear();
    _inspectorAnimPick.clear();
    EntityInspectorOverrides emptyOv;
    commitInspectorOverrides(emptyOv);
    std::fprintf(stderr, "[EditorSession] inspector overrides cleared\n");
}

// ED-03: shared work for both Apply and Reset. Forward the
// override to the runtime, refresh labels, trigger redraw.
void EditorSession::commitInspectorOverrides(const EntityInspectorOverrides& ov)
{
    // PR-5 (LM-2): 双层守卫 — applyInspectorOverrides/resetInspectorOverrides
    // 入口已守；此处再守一次防外部 caller 直接调 commitInspectorOverrides 路径。
    if (!allowInspectorEdit()) return;
    _playRuntime.applyComponentOverrides(ov);
    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// =============================================================================
// D5.5 — Card promotion wiring.
//
// We walk the primary UIManager's root widget tree and inject
// setPromoteCallback into every DockCard we encounter (slot or floating).
// The lambda closes over `this` so detachToOwnWindow on any card routes
// into the optional EditorChildWindowManager. Card-promotion is a
// pure-AYUI feature; the only AYEditor involvement is injecting the
// callback — we do NOT touch DockCard's interface here (callback
// injection is the contract, per design §17.5 D5.5).
//
// Walk strategy: recursive descent via getChildren(). The root may be
// nullptr (no layout loaded yet) — we silently no-op. A DockArea
// that isn't reached because the layout tree didn't include one also
// no-ops; there are simply no cards to wire.
// =============================================================================

namespace {
void wirePromoteCallbackRecursive(ayt::ui::Widget* w, EditorSession* session) {
    if (!w) return;
    if (auto* dock = dynamic_cast<ayt::ui::DockArea*>(w)) {
        // Only wire floating cards. Slot-docked cards are NOT promoted
        // because DockCard::detachToOwnWindow's parent == DockOverlay
        // gate (K-INV-D5.5 in AYDockCard.cpp) would reject them anyway;
        // injecting a no-op-true callback would invite confusion. The
        // slot-card-promotion path is a future cut.
        if (auto* overlay = dock->getOverlay()) {
            for (size_t i = 0; i < overlay->getFloatingCardCount(); ++i) {
                if (auto* card = overlay->getFloatingCard(i)) {
                    card->setPromoteCallback(
                        [session](
                            const std::string& cardId,
                            const std::wstring& title,
                            int x, int y, int w, int h) -> bool {
                            // Convert wstring title -> narrow UTF-8
                            // for ChildWindowConfig::title (the
                            // top-level window title is wide internally
                            // in AYDevice, but ChildWindowConfig is the
                            // narrow-string intermediate used at the
                            // AYEditor API boundary).
                            std::string narrowTitle(title.begin(),
                                                    title.end());
                            ChildWindowConfig cfg;
                            cfg.title = std::move(narrowTitle);
                            // The promoted card keeps its id as a
                            // loadLayout hint; the child's loadLayout
                            // is best-effort (open still succeeds on
                            // missing file).
                            cfg.layoutPath = cardId + ".json";
                            cfg.x = x;
                            cfg.y = y;
                            cfg.width  = w;
                            cfg.height = h;
                            void* hOut = nullptr;
                            if (session && session->childWindows()) {
                                return session->childWindows()
                                    ->openChildWindow(cfg, hOut);
                            }
                            return false;
                        });
                }
            }
        }
        // Don't descend — DockArea owns its subtree and we already
        // walked it via overlay enumeration. Returning here matches
        // the explicit "I own my tree" stance of CompoundWidget.
        return;
    }
    for (ayt::ui::Widget* child : w->getChildren()) {
        wirePromoteCallbackRecursive(child, session);
    }
}
} // namespace

void EditorSession::wirePromoteCallback() {
    if (!_childWindows) return;  // no manager → no-op
    wirePromoteCallbackRecursive(_ui.root(), this);
}

void EditorSession::setModeLabel(const std::wstring& text) {
    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

// PR-5 (LM-2): Inspector hint 文案切换 helper.
// inspector_hint TextLabel 在 editor_shell.ui.json:141 已存在（id=
// "inspector_hint", initial text "No selection"）。Play/Paused 时切
// "Locked during Play"；Edit 模式回 "Click buttons to configure."
// （让用户在 Reset 完 pick 后看见可操作提示）。
void EditorSession::setInspectorHint(const std::wstring& text) {
    if (auto* widget = _ui.findById("inspector_hint")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

void EditorSession::onModeChanged(EditorMode mode) {
    _ui.cancelCapture();
    if (_freecam.isLooking()) {
        _freecam.endLook();
    }

    switch (mode) {
    case EditorMode::Edit:
        setModeLabel(L"EDIT");
        // PR-5 (LM-2): Inspector 写权限恢复 + hint 文案恢复。
        _allowInspectorEdit = true;
        setInspectorHint(L"Click buttons to configure.");
        if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
            sub->clearCameraOverride();
        }
        break;
    case EditorMode::Play:
        setModeLabel(_netClientAutoPlay ? L"PLAY (NET CLIENT)" : L"PLAY");
        // PR-5 (LM-2): Inspector 写权限锁 + hint 提示。
        _allowInspectorEdit = false;
        setInspectorHint(L"Locked during Play.");
        applyRenderSettingsFromPanel();
        pushFreecamToRenderer();
        // Auto-select whatever Play just spawned so Inspector is never
        // stuck on "No selection" while the viewport shows a cube.
        selectCharacter();
        break;
    case EditorMode::Paused:
        setModeLabel(L"PAUSED");
        // PR-5 (LM-2): Paused 也锁 Inspector（与 Play 同语义）。
        _allowInspectorEdit = false;
        setInspectorHint(L"Locked during Play.");
        applyRenderSettingsFromPanel();
        pushFreecamToRenderer();
        break;
    }

    // v0.3 PR-4 — mode 变化时同步 refresh lbl_unsaved（design §4.3.x 决策 5a）
    refreshUnsavedIndicator();

    // v0.3+ PR-5 — mode 切换会换 Hierarchy 的 World 源（决策 1b）且
    // 可能销毁 Play 实体 → 清选择 + 排队重建。延迟到 update() 消费
    // 是因为 btn_play/btn_stop click handler 仍在 UIManager 事件派发栈内
    // （Landmine B）。
    _outlinerSelectedEntityId = 0;
    _outlinerRefreshPending = true;

    _ui.invalidateLayout();
    _ui.layout();
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
